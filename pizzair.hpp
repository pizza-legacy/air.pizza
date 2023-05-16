#include "common.hpp"
#include "memo.hpp"
#include "pizzalend.hpp"

#define PSYM_LEN 3

#define LPSYM_MAX_SUPPLY 10000000000

#define ANY_LPSYM symbol_code("ANY")

#define LEVERAGE_DECIMALS 4

#ifdef MAINNET
  #define LPTOKEN_CONTRACT name("lptoken.air")
  #define PREMIUM_ACCOUNT name("income.air")
  #define PLANB_CONTRACT name("planb.air")
  #define LOG_CONTRACT name("log.pizza")
  #define ADMIN_ACCOUNT name("admin.pizza")
#else
  #define LPTOKEN_CONTRACT name("lptokencode1")
  #define PREMIUM_ACCOUNT name("premiumacc11")
  #define PLANB_CONTRACT name("airplanbcode")
  #define LOG_CONTRACT name("logcontract2")
  #define ADMIN_ACCOUNT name("lendadmacc11")
#endif

#define FEATURE_SWAP name("swap")
#define FEATURE_SUPPLY name("supply")
#define FEATURE_DEMAND name("demand")

#define ALL name("all")

namespace pizzair {
  double p_to_q(double p, double A, double x, double y) {
    double n = 4*(4*A - 1)*x*y;
    double m = -16*A*x*y*(x+y);
    double t = sqrt(pow(m/2, 2)+pow(n/3, 3));
    double D = cbrt(-m/2 + t) + cbrt(-m/2 - t);
    double G = 4*A*(x+y+p-D) + D;
    double c = 4*y*G - pow(D, 3)/(x+p);
    double a = 16*A;
    double b = -(4*G + 16*A*y);
    double delta = pow(b, 2) - 4*a*c;
    double q = (-b - sqrt(delta)) / (2*a);
    return q;
  };

  double cal_price(double A, double x, double y) {
    double p = std::min(x, y) * pow(10, -6);
    return p_to_q(p, A, x, y) / p;
  };

  struct market_config {
    uint32_t leverage;
    decimal fee_rate;
  };

  class [[eosio::contract]] pizzair : public contract {
  public:
    pizzair(name self, name first_receiver, datastream<const char*> ds) :
      contract(self, first_receiver, ds), pools(self, self.value), 
      markets(self, self.value), liqdts(self, self.value), mleverages(self, self.value), 
      mfees(self, self.value), invitations(self, self.value), minsupplies(self, self.value) {}

    [[eosio::on_notify("*::transfer")]]
    void on_transfer(name from, name to, asset quantity, std::string memo);

    [[eosio::action]]
    void addpool(symbol_code psym, uint8_t decimals);

    [[eosio::action]]
    void addmarket(symbol_code psym, extended_symbol sym0, extended_symbol sym1, market_config config);

    [[eosio::action]]
    void setmarket(symbol_code lpsym, market_config config);

    [[eosio::action]]
    void setlendable(symbol_code lpsym, extended_symbol sym, bool lendable);

    [[eosio::action]]
    void setleverage(symbol_code lpsym, uint32_t leverage, uint32_t effective_secs);

    [[eosio::action]]
    void setfee(symbol_code lpsym, decimal lp_rate, int index);

    [[eosio::action]]
    void addallow(name account, name feature, uint32_t duration);

    [[eosio::action]]
    void addallows(std::vector<name> accounts, name feature, uint32_t duration);

    [[eosio::action]]
    void remallow(name account, name feature);

    [[eosio::action]]
    void supply(name account, symbol_code lpsym);

    [[eosio::action]]
    void setinvite(name code, name account, decimal fee_rate);

    [[eosio::action]]
    void setminsupply(symbol_code psym, uint64_t amount);

    #ifndef MAINNET
      [[eosio::action]]
      void clear();
    #endif

  private:
    void _log(name event, std::vector<std::string> args) {
      uint64_t millis = current_millis();
      action(
        permission_level{_self, name("active")},
        LOG_CONTRACT,
        name("log"),
        std::make_tuple(_self, event, args, millis)
      ).send();
    };

    void _log_swap(name account, symbol_code lpsym, asset quantity, asset got, asset fee) {
      std::vector<std::string> args = {account.to_string(), lpsym.to_string(), quantity.to_string(), got.to_string(), fee.to_string()};
      _log(name("swap"), args);
    };

    void _log_supply(name account, symbol_code lpsym, asset deposits[2], asset lpquantity) {
      std::vector<std::string> args = {account.to_string(), lpsym.to_string(), deposits[0].to_string(), deposits[1].to_string(), lpquantity.to_string()};
      _log(name("supply"), args);
    };

    void _log_demand(name account, symbol_code lpsym, asset lpquantity, std::vector<asset> gots) {
      std::vector<std::string> args = {account.to_string(), lpsym.to_string(), lpquantity.to_string(), gots[0].to_string(), gots[1].to_string()};
      _log(name("demand"), args);
    };

    void _log_upmarket(symbol_code lpsym, std::vector<asset> reserves, std::vector<double> prices, uint64_t lpamount) {
      std::vector<std::string> args = {lpsym.to_string(), reserves[0].to_string(), reserves[1].to_string(), std::to_string(prices[0]), std::to_string(prices[1]), std::to_string(lpamount)};
      _log(name("upmarket"), args);
    };

    void _log_upliqdt(name account, symbol_code lpsym, asset lpquantity) {
      std::vector<std::string> args = {account.to_string(), lpsym.to_string(), lpquantity.to_string()};
      _log(name("upliqdt"), args);
    };

    struct [[eosio::table]] pool {
      symbol_code psym;
      uint8_t decimals;

      uint64_t primary_key() const {
        return psym.raw();
      }
    };

    typedef eosio::multi_index<name("pool"), pool> pool_tlb;
    pool_tlb pools;

    symbol _next_lptoken(pool p);

    struct [[eosio::table]] minsupply {
      symbol_code psym;
      uint64_t amount;

      uint64_t primary_key() const {
        return psym.raw();
      }
    };

    typedef eosio::multi_index<name("minsupply"), minsupply> minsupply_tlb;
    minsupply_tlb minsupplies;

    uint64_t get_minsupply(symbol_code psym, uint8_t precision) {
      auto itr = minsupplies.find(psym.raw());
      if (itr != minsupplies.end()) {
        return itr->amount;
      }
      
      return pow(10, precision);
    };

    struct [[eosio::table]] market_leverage {
      symbol lptoken;
      uint32_t leverage;
      uint32_t begined_at;
      uint32_t effective_secs;

      uint64_t primary_key() const {
        return lptoken.code().raw();
      };
    };

    typedef eosio::multi_index<name("mleverage"), market_leverage> mleverage_tlb;
    mleverage_tlb mleverages;

    struct [[eosio::table]] market {
      symbol lptoken;
      std::vector<extended_symbol> syms;
      std::vector<asset> reserves;
      std::vector<double> prices;
      std::vector<uint8_t> lendables;
      uint64_t lpamount;
      market_config config;

      uint64_t primary_key() const {
        return lptoken.code().raw();
      }

      symbol_code psym() const {
        return symbol_code(lptoken.code().to_string().substr(0, PSYM_LEN));
      };

      uint64_t by_psym() const {
        return psym().raw();
      }

      market reverse() const {
        market s;
        s.lptoken = lptoken;
        s.syms[0] = syms[1];
        s.syms[1] = syms[0];
        s.reserves[0] = reserves[1];
        s.reserves[1] = reserves[0];
        s.prices[0] = prices[1];
        s.prices[1] = prices[0];
        return s;
      }
    };

    typedef eosio::multi_index<
      name("market"), market,
      indexed_by<name("bypsym"), const_mem_fun<market, uint64_t, &market::by_psym>>
    > market_tlb;
    market_tlb markets;

    market_tlb::const_iterator _find_market(extended_symbol sym0, extended_symbol sym1) {
      for (auto itr = markets.begin(); itr != markets.end(); itr++) {
        if (itr->syms[0] == sym0 && itr->syms[1] == sym1) return itr;
        if (itr->syms[1] == sym0 && itr->syms[0] == sym1) return itr;
      }
      return markets.end();
    };

    market _get_market(extended_symbol sym0, extended_symbol sym1) {
      auto itr = _find_market(sym0, sym1);
      check(itr != markets.end(), "market not found");
      if (itr->syms[0] == sym0 && itr->syms[1] == sym1) return *itr;
      return itr->reverse();
    };

    void _update_market_reserve(market_tlb::const_iterator mitr, std::vector<asset> st_reserves, std::vector<asset> reserves, uint64_t lpamount);

    uint32_t _get_leverage(market_tlb::const_iterator mitr) {
      int leverage_precision = pow(10, LEVERAGE_DECIMALS);

      if (mitr->config.leverage < leverage_precision/100) {
        markets.modify(mitr, _self, [&](auto& row) {
          row.config.leverage *= leverage_precision;
        });
      }

      auto litr = mleverages.find(mitr->lptoken.code().raw());
      if (litr == mleverages.end()) return mitr->config.leverage;

      if (litr->leverage < leverage_precision/100) {
        mleverages.modify(litr, _self, [&](auto& row) {
          row.leverage *= leverage_precision;
        });
      }
      
      uint32_t passed_secs = current_secs() - litr->begined_at;
      print_f("passed secs: %, effective secs: %, ", passed_secs, litr->effective_secs);
      if (passed_secs >= litr->effective_secs) {
        uint32_t leverage = litr->leverage;
        markets.modify(mitr, _self, [&](auto& row) {
          row.config.leverage = leverage;
        });
        mleverages.erase(litr);
        return leverage;
      }

      int32_t a1 = mitr->config.leverage;
      int32_t a2 = litr->leverage;
      int32_t diff = a2 - a1;
      double t = passed_secs;
      double T = litr->effective_secs;
      double rate = t/T;

      print_f("a1: %, a2: %, diff: %, ", a1, a2, diff);
      int32_t A = a1 + diff*rate;
      print_f("A: %", A);
      return A;
    };

    double _get_exact_leverage(market_tlb::const_iterator mitr) {
      uint32_t A = _get_leverage(mitr);
      return (double) A / pow(10, LEVERAGE_DECIMALS);
    }

    struct [[eosio::table]] market_fee {
      symbol lptoken;
      decimal lp_rate;
      int index;

      uint64_t primary_key() const {
        return lptoken.code().raw();
      }
    };
    typedef eosio::multi_index<name("mfee"), market_fee> mfee_tlb;
    mfee_tlb mfees;

    market_fee _get_fee_conf(symbol lptoken) {
      auto itr = mfees.find(lptoken.code().raw());
      if (itr != mfees.end()) return *itr;

      decimal lp_rate = double2decimal(0.5);
      int index = 0;
      itr = mfees.emplace(_self, [&](auto& row) {
        row.lptoken = lptoken;
        row.lp_rate = lp_rate;
        row.index = index;
      });
      return *itr;
    };

    struct [[eosio::table]] order {
      name account;
      std::vector<asset> reserves;

      uint64_t primary_key() const {
        return account.value;
      }
    };
    typedef eosio::multi_index<name("order"), order> order_tlb;

    struct [[eosio::table]] liqdt {
      uint64_t id;
      name account;
      symbol lptoken;
      std::vector<asset> reserves;
      uint64_t lpamount;

      uint64_t by_account() const {
        return account.value;
      }

      uint64_t by_lpsymbol() const {
        return lptoken.code().raw();
      }

      uint128_t by_acc_lpsym() const {
        return raw(account.value, lptoken.code().raw());
      }

      uint64_t primary_key() const {
        return id;
      }
    };
    typedef eosio::multi_index<
      name("liqdt"), liqdt,
      indexed_by<name("byaccount"), const_mem_fun<liqdt, uint64_t, &liqdt::by_account>>,
      indexed_by<name("bylpsymbol"), const_mem_fun<liqdt, uint64_t, &liqdt::by_lpsymbol>>,
      indexed_by<name("byacclpsym"), const_mem_fun<liqdt, uint128_t, &liqdt::by_acc_lpsym>>
    > liqdt_tlb;
    liqdt_tlb liqdts;

    void _incr_liqdt(name account, market_tlb::const_iterator mitr, std::vector<asset> deposits, uint64_t lpamount) {
      auto liqdts_byacclpsym = liqdts.get_index<name("byacclpsym")>();
      auto itr = liqdts_byacclpsym.find(raw(account.value, mitr->lptoken.code().raw()));

      if (itr == liqdts_byacclpsym.end()) {
        liqdts.emplace(_self, [&](auto& row) {
          row.id = liqdts.available_primary_key();
          row.account = account;
          row.lptoken = mitr->lptoken;
          row.reserves = deposits;
          row.lpamount = lpamount;
        });
        _log_upliqdt(account, mitr->lptoken.code(), asset(lpamount, mitr->lptoken));
      } else {
        liqdts_byacclpsym.modify(itr, _self, [&](auto& row) {
          row.reserves[0] += deposits[0];
          row.reserves[1] += deposits[1];
          row.lpamount += lpamount;
        });
        _log_upliqdt(account, mitr->lptoken.code(), asset(itr->lpamount, mitr->lptoken));
      }
    };

    void _decr_liqdt(name account, market_tlb::const_iterator mitr, uint64_t lpamount) {
      auto liqdts_byacclpsym = liqdts.get_index<name("byacclpsym")>();
      auto itr = liqdts_byacclpsym.find(raw(account.value, mitr->lptoken.code().raw()));

      check(itr != liqdts_byacclpsym.end() && itr->lpamount >= lpamount, "insufficient account lpamount");
      if (itr->lpamount == lpamount) {
        liqdts_byacclpsym.erase(itr);
        _log_upliqdt(account, mitr->lptoken.code(), asset(0, mitr->lptoken));
      } else {
        double reduct_ratio = (double)lpamount / itr->lpamount;
        liqdts_byacclpsym.modify(itr, _self, [&](auto& row) {
          row.reserves[0].amount *= (1 - reduct_ratio);
          row.reserves[1].amount *= (1 - reduct_ratio);
          row.lpamount -= lpamount;
        });
        _log_upliqdt(account, mitr->lptoken.code(), asset(itr->lpamount, mitr->lptoken));
      }
    };

    struct [[eosio::table]] invitation {
      name code;
      name account;
      decimal fee_rate;

      uint64_t primary_key() const { return code.value; }

      bool is_valid() {
        return is_account(account) && fee_rate.amount > 0;
      };
    };

    typedef eosio::multi_index<name("invitation"), invitation> invitation_tlb;
    invitation_tlb invitations;

    invitation _get_invitation(name code) {
      auto itr = invitations.find(code.value);
      if (itr != invitations.end()) return *itr;

      invitation ivt;
      ivt.code = code;
      return ivt;
    };

    void _deposit(symbol_code lpsym, name account, name contract, asset quantity);

    void _create_lptoken(asset maximum_supply);

    void _issue_lptoken(name to, asset quantity);

    void _retire_lptoken(asset quantity);

    void _swap(symbol_code lpsym, name account, name contract, asset quantity, uint64_t expect = 0, uint32_t slippage = 0, invitation ivt = invitation());

    void _demand(name account, name contract, asset quantity, int sym_index = -1);

    void _transfer_out(name to, name contract, asset quantity, std::string memo);

    void _setlendable(market_tlb::const_iterator mitr, int index, bool lendable);

    enum AllowType {
      ManualAllow = 1
    };

    struct [[eosio::table]] allowlist {
      name account;
      uint8_t type;
      uint64_t expired_at;

      uint64_t primary_key() const { return account.value; }
    };
    typedef eosio::multi_index<name("allowlist"), allowlist> allowlist_tlb;

    bool _in_allowlist(name account, name feature = ALL) {
      allowlist_tlb allows(_self, feature.value);
      auto itr = allows.find(account.value);
      if (itr == allows.end()) return false;
      if (itr->expired_at > 0 && itr->expired_at <= current_millis()) {
        allows.erase(itr);
        return false;
      }
      return true;
    };

    void _addto_allowlist(name account, name feature = ALL, uint8_t type = AllowType::ManualAllow, uint32_t duration = 0) {
      uint64_t expired_at = 0;
      if (duration > 0) {
        expired_at = current_millis() + duration * 1000;
      }
      allowlist_tlb allows(_self, feature.value);
      auto itr = allows.find(account.value);
      if (itr == allows.end()) {
        allows.emplace(_self, [&](auto& row) {
          row.account = account;
          row.type = type;
          row.expired_at = expired_at;
        });
      } else {
        allows.modify(itr, _self, [&](auto& row) {
          row.type = type;
          row.expired_at = expired_at;
        });
      }
    };

    bool _isblock(name account, name feature) {
      if (_in_allowlist(ALL, ALL)) return false;
      if (feature != ALL) {
        if (_in_allowlist(ALL, feature)) return false;
      }
      if (_in_allowlist(account, ALL)) return false;
      if (feature != ALL) {
        if (_in_allowlist(account, feature)) return false;
      }

      return true;
    };

    void _check_allow(name account, name fname) {
      check(!_isblock(account, fname), "account is blocked");
    };

    void _on_lptoken_transfer(name from, name to, asset quantity, std::string memo);
  };
}