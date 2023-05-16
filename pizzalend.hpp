#include "common.hpp"

#ifdef MAINNET
  #define LEND_CONTRACT name("lend.pizza")
  #define WALLET_ACCOUNT name("vault.pizza")
#else
  #define LEND_CONTRACT name("lendcontract")
  #define WALLET_ACCOUNT name("lendwallet11")
#endif

namespace pizzalend {
  struct pztoken_config {
    decimal base_rate;
    decimal max_rate;
    decimal base_discount_rate;
    decimal max_discount_rate;
    decimal best_usage_rate;
    decimal floating_fee_rate;
    decimal fixed_fee_rate;
    decimal liqdt_rate;
    decimal liqdt_bonus;
    decimal max_ltv;
    decimal floating_rate_power;
    bool is_collateral;
    bool can_stable_borrow;
    uint8_t borrow_liqdt_order;
    uint8_t collateral_liqdt_order;
  };

  struct [[eosio::table]] pztoken {
    name pzname;
    extended_symbol pzsymbol;
    extended_symbol anchor;
    asset cumulative_deposit;
    asset available_deposit;
    asset pzquantity;
    asset borrow;
    asset cumulative_borrow;
    asset variable_borrow;
    asset stable_borrow;
    decimal usage_rate;
    decimal floating_rate;
    decimal discount_rate;
    decimal price;
    double pzprice;
    double pzprice_rate;
    uint64_t updated_at;
    pztoken_config config;

    uint128_t by_pzsymbol() const {
      return raw(pzsymbol);
    }

    uint128_t by_anchor() const {
      return raw(anchor);
    }

    uint64_t by_borrow_liqdt_order() const {
      return config.borrow_liqdt_order;
    }

    uint64_t by_collateral_liqdt_order() const {
      return config.collateral_liqdt_order;
    }

    uint64_t primary_key() const { return pzname.value; }

    double cal_usage_rate() const {
      double d = asset2double(available_deposit);
      double b = asset2double(borrow);
      if (b + d == 0) return 0;
      return  b / (b + d);
    };

    double cal_pzprice() const {
      uint64_t now = current_millis();
      uint64_t secs = (now - updated_at) / 1000;
      return pzprice * (1 + pzprice_rate * secs);
    };

    asset cal_pzquantity(asset quantity, double pzprice = 0) {
      check(quantity.symbol == anchor.get_symbol(), "attempt to calculate pzquantity with different anchor symbol");
      if (pzprice == 0) {
        pzprice = cal_pzprice();
      }
      asset pzquantity = asset(0, pzsymbol.get_symbol());
      pzquantity.amount = asset2double(quantity) * pow(10, pzquantity.symbol.precision()) / pzprice;
      return pzquantity;
    }

    asset cal_anchor_quantity(asset pzquantity, double pzprice = 0) {
      check(pzquantity.symbol == pzsymbol.get_symbol(), "attempt to calculate anchor quantity with different pz symbol");
      if (pzprice == 0) {
        pzprice = cal_pzprice();
      }
      asset quantity = asset(0, anchor.get_symbol());
      quantity.amount = asset2double(pzquantity) * pow(10, quantity.symbol.precision()) * pzprice;
      return quantity;
    }
  };

  typedef eosio::multi_index<
    name("pztoken"), pztoken,
    indexed_by<name("bypzsymbol"), const_mem_fun<pztoken, uint128_t, &pztoken::by_pzsymbol>>,
    indexed_by<name("byanchor"), const_mem_fun<pztoken, uint128_t, &pztoken::by_anchor>>,
    indexed_by<name("sortbyliqdt"), const_mem_fun<pztoken, uint64_t, &pztoken::by_borrow_liqdt_order>>,
    indexed_by<name("sortbycoll"), const_mem_fun<pztoken, uint64_t, &pztoken::by_collateral_liqdt_order>>
  > pztoken_tlb;

  pztoken_tlb pztokens(LEND_CONTRACT, LEND_CONTRACT.value);

  pztoken get_pztoken_byanchor(extended_symbol anchor) {
    auto pztokens_byanchor = pztokens.get_index<name("byanchor")>();
    std::string msg = "pztoken with anchor " + anchor.get_symbol().code().to_string() + " not found";
    return pztokens_byanchor.get(raw(anchor), msg.c_str());
  };
  
  pztoken get_pztoken(name pzname) {
    return pztokens.get(pzname.value, "pztoken not found");
  };
}