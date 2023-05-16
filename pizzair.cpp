#include "pizzair.hpp"

namespace pizzair {
  void pizzair::on_transfer(name from, name to, asset quantity, std::string s) {
    if (from != _self && to != _self && get_first_receiver() == LPTOKEN_CONTRACT) {
      return _on_lptoken_transfer(from, to, quantity, s);
    }

    if (from == _self || to != _self) return;
    if (from == LEND_CONTRACT || from == WALLET_ACCOUNT) return;

    memo m = memo(s);
    std::string first = m.get(0);
    if (first == "deposit") {
      symbol_code lpsym = symbol_code(m.get(1));
      _deposit(lpsym, from, get_first_receiver(), quantity);
    } else if (first == "swap") {
      symbol_code lpsym = symbol_code(m.get(1));
      uint64_t expect = 0;
      uint32_t slippage_protection = 0;
      if (m.get(2) != "") {
        expect = atol(m.get(2).c_str());
        slippage_protection = atoi(m.get(3).c_str());
        check(slippage_protection >= 10 && slippage_protection <= 500, "slippage protection should be between 1‰ and 5%");
      }

      std::string invite_code = m.get(4);
      invitation ivt = _get_invitation(name(invite_code));
      _swap(lpsym, from, get_first_receiver(), quantity, expect, slippage_protection, ivt);
    } else if (first == "demand") {
      int sym_index = -1;
      if (m.get(1) != "") {
        sym_index = atoi(m.get(1).c_str());
        check(sym_index == 0 || sym_index == 1, "invalid symbol index");
      }
      _demand(from, get_first_receiver(), quantity, sym_index);
    } else {
      check(false, "invalid memo for air");
    }
  };

  void pizzair::supply(name account, symbol_code lpsym) {
    require_auth(account);

    _check_allow(account, FEATURE_SUPPLY);

    auto mitr = markets.find(lpsym.raw());
    check(mitr != markets.end(), "market not found");

    order_tlb orders(_self, lpsym.raw());
    auto itr = orders.find(account.value);
    check(itr != orders.end() && (itr->reserves[0].amount > 0 || itr->reserves[1].amount > 0), "not yet deposited");

    asset deposits[2] = {itr->reserves[0], itr->reserves[1]};
    double deposit_rs[2] = {asset2double(itr->reserves[0]), asset2double(itr->reserves[1])};
    if (mitr->lpamount == 0) {
      check(deposits[0].amount > 0 && deposits[1].amount > 0, "must deposited all tokens for first supply");
    }

    std::vector<asset> reserves = mitr->reserves;
    std::vector<asset> st_reserves = {asset(0, mitr->syms[0].get_symbol()), asset(0, mitr->syms[1].get_symbol())};
    std::vector<asset> addeds = {asset(0, reserves[0].symbol), asset(0, reserves[1].symbol)};

    for (auto i = 0; i < 2; i++) {
      if (mitr->lendables[i]) {
        pizzalend::pztoken pz = pizzalend::get_pztoken_byanchor(mitr->syms[i]);
        double pzprice = pz.cal_pzprice();

        st_reserves[i] = pz.cal_anchor_quantity(reserves[i], pzprice);
        addeds[i] = pz.cal_pzquantity(deposits[i], pzprice);
        if (deposits[i].amount > 0) {
          _transfer_out(LEND_CONTRACT, mitr->syms[i].get_contract(), deposits[i], "collateral");
        }
      } else {
        st_reserves[i] = reserves[i];
        addeds[i] = deposits[i];
      }
    }

    print_f("added0: %, add1: % | ", addeds[0], addeds[1]);

    std::vector<double> raw_rs = {asset2double(st_reserves[0]), asset2double(st_reserves[1])};
    
    double raw_ratio = 1;
    if (raw_rs[1] > 0) {
      raw_ratio = raw_rs[0] / raw_rs[1];
    }

    int extra_index = -1;
    double extra_rs = 0;
    double standard_deposit_rs[2] = {deposit_rs[0], deposit_rs[1]};
    if (deposit_rs[1] == 0) {
      extra_index = 0;
      extra_rs = deposit_rs[0];
      standard_deposit_rs[0] = 0;
    } else if (deposit_rs[0] == 0) {
      extra_index = 1;
      extra_rs = deposit_rs[1];
      standard_deposit_rs[1] = 0;
    } else {
      double deposit_ratio = deposit_rs[0] / deposit_rs[1];
      if (deposit_ratio > raw_ratio) {
        extra_index = 0;
        extra_rs = deposit_rs[1] * (deposit_ratio - raw_ratio);
        standard_deposit_rs[0] -= extra_rs;
      } else if (deposit_ratio < raw_ratio) {
        extra_index = 1;
        extra_rs = deposit_rs[0] * (1/deposit_ratio - 1/raw_ratio);
        standard_deposit_rs[1] -= extra_rs;
      }
    }

    int64_t standard_lpamount = 0;
    if (mitr->lpamount == 0) {
      check(standard_deposit_rs[0] == standard_deposit_rs[1], "must deposit the same amount for first supply");
      standard_lpamount = standard_deposit_rs[0] * pow(10, mitr->lptoken.precision()) * 2;
    } else {
      standard_lpamount = standard_deposit_rs[0] / raw_rs[0] * mitr->lpamount;
    }

    print_f("standard - deposit0: %, deposit1: %, lpamount: % | ", standard_deposit_rs[0], standard_deposit_rs[1], standard_lpamount);

    double A = _get_exact_leverage(mitr);
    print_f("current A: % | ", A);

    print_f("extra index: %, extra_rs: % | ", extra_index, extra_rs);

    int64_t extra_lpamount = 0;
    int64_t extra_amount = extra_rs * pow(10, deposits[extra_index].symbol.precision());

    if (extra_rs <= 0.0001) {
      addeds[extra_index].amount *= (1 - (double)extra_amount/deposits[extra_index].amount);
      deposits[extra_index].amount -= extra_amount;
    } else {
      if (extra_index >= 0 && extra_amount > 1) {
        int other_index = extra_index == 0 ? 1 : 0;

        double latest_rs[2] = {raw_rs[0] + standard_deposit_rs[0], raw_rs[1] + standard_deposit_rs[1]};
        check(extra_rs <= latest_rs[extra_index], "failed to add liquidity due to pool disproportion");
        double x = latest_rs[extra_index];
        double y = latest_rs[other_index];
        double n = extra_rs;

        double max_price = cal_price(A, x, y);
        double min_price = cal_price(A, x + n, y);

        print_f("max price: %, min price: % | ", max_price, min_price);

        int times = 0;
        double price, p, q;
        while (true) {
          times++;
          check(times <= 10, "invalid input, please add the coins in a balanced proportion");
          price = (max_price + min_price) / 2;
          print_f("#%, price: % | ", times, price);
          p = y * n / ((x + n) * price + y);
          q = p_to_q(p, A, x, y);
          double verify = q * (x + p) / ((n - p) * (y - q)) - 1;
          if (verify >= 0.0005) {
            min_price = price;
          } else if (verify < 0) {
            max_price = price;
          } else {
            break;
          }
        }
        extra_lpamount = (n - p) / (x + p) * (mitr->lpamount + standard_lpamount);
        print_f("p: %, q: %, extra lpamount: % | ", p, q, extra_lpamount);
      }
    }

    print_f("added0: %, add1: % | ", addeds[0], addeds[1]);
    
    for (auto i = 0; i < 2; i++) {
      st_reserves[i] += deposits[i];
    }
    std::vector<double> rs = {asset2double(st_reserves[0]), asset2double(st_reserves[1])};

    double price0 = cal_price(A, rs[0], rs[1]);
    double price1 = cal_price(A, rs[1], rs[0]);
    
    int64_t lpamount = standard_lpamount + extra_lpamount;
    uint64_t minsupply = get_minsupply(mitr->psym(), mitr->lptoken.precision());
    check(lpamount >= minsupply, "supply amount is too small");

    asset lpquantity = asset(lpamount, mitr->lptoken);
    _log_supply(account, mitr->lptoken.code(), deposits, lpquantity);

    markets.modify(mitr, _self, [&](auto& row) {
      row.reserves[0] += addeds[0];
      row.reserves[1] += addeds[1];
      row.prices[0] = price0;
      row.prices[1] = price1;
      row.lpamount += lpamount;
    });

    _log_upmarket(mitr->lptoken.code(), st_reserves, mitr->prices, mitr->lpamount);

    std::vector<asset> principals = {asset(0, st_reserves[0].symbol), asset(0, st_reserves[1].symbol)};
    double ratio = (double)lpamount / mitr->lpamount;
    principals[0].amount = st_reserves[0].amount * ratio;
    principals[1].amount = st_reserves[1].amount * ratio;
    _incr_liqdt(account, mitr, principals, lpamount);
    
    _issue_lptoken(account, lpquantity);

    orders.erase(itr);
  };

  void pizzair::_create_lptoken(asset maximum_supply) {
    action(
      permission_level{LPTOKEN_CONTRACT, name("active")},
      LPTOKEN_CONTRACT,
      name("create"),
      std::make_tuple(_self, maximum_supply)
    ).send();
  };

  void pizzair::_issue_lptoken(name to, asset quantity) {
    action(
      permission_level{_self, name("active")},
      LPTOKEN_CONTRACT,
      name("issue"),
      std::make_tuple(to, quantity, std::string("issue lptoken for air"))
    ).send();
  };

  void pizzair::_retire_lptoken(asset quantity) {
    action(
      permission_level{_self, name("active")},
      LPTOKEN_CONTRACT,
      name("retire"),
      std::make_tuple(_self, quantity, std::string("retire lptoken"))
    ).send();
  };

  void pizzair::_deposit(symbol_code lpsym, name account, name contract, asset quantity) {
    _check_allow(account, FEATURE_SUPPLY);

    market m = markets.get(lpsym.raw(), "market not found");

    order_tlb orders(_self, lpsym.raw());
    auto itr = orders.find(account.value);
    if (itr == orders.end()) {
      itr = orders.emplace(_self, [&](auto& row) {
        row.account = account;
        row.reserves = {asset(0, m.syms[0].get_symbol()), asset(0, m.syms[1].get_symbol())};
      });
    }

    int index = -1;
    for (int i = 0; i <= 1; i++) {
      if (m.syms[i].get_contract() == contract && m.syms[i].get_symbol() == quantity.symbol) {
        index = i;
      }
    }
    check(index >= 0, "market does not match");
    check(itr->reserves[index].amount == 0, "already deposit this token");
    orders.modify(itr, _self, [&](auto& row) {
      row.reserves[index] = quantity;
    });
  };

  void pizzair::_swap(symbol_code lpsym, name account, name contract, asset quantity, uint64_t expect, uint32_t slippage, invitation ivt) {
    _check_allow(account, FEATURE_SWAP);

    auto mitr = markets.find(lpsym.raw());
    check(mitr != markets.end(), "market not found");

    std::vector<asset> reserves = mitr->reserves;

    std::vector<asset> st_reserves = {asset(0, mitr->syms[0].get_symbol()), asset(0, mitr->syms[1].get_symbol())};

    extended_symbol sym = extended_symbol(quantity.symbol, contract);

    int in_index = -1;
    for (int i = 0; i <= 1; i++) {
      if (mitr->syms[i] == sym) {
        in_index = i;
      }
    }
    check(in_index >= 0, "market does not match");

    asset from_quantity = quantity;
    
    int out_index = in_index == 0 ? 1 : 0;

    double fee_rate = decimal2double(mitr->config.fee_rate);
    market_fee fee_conf = _get_fee_conf(mitr->lptoken);

    asset fee = asset(0, mitr->syms[fee_conf.index].get_symbol());
    asset admin_fee = asset(0, fee.symbol);

    double invite_fee_rate = decimal2double(ivt.fee_rate);
    asset invite_fee = asset(0, mitr->syms[fee_conf.index].get_symbol());
    
    if (fee_conf.index == in_index) {
      fee.amount = (double)from_quantity.amount * fee_rate;
      admin_fee.amount = (double)fee.amount * (1 - decimal2double(fee_conf.lp_rate));
      invite_fee.amount = (double)from_quantity.amount * invite_fee_rate;
      from_quantity -= fee;
    }
    
    double p, q, A;
    p = asset2double(from_quantity);
    A = _get_exact_leverage(mitr);

    asset st_incr = from_quantity;
    if (fee_conf.index == in_index) {
      st_incr += (fee - admin_fee);
    }
    asset incr = asset(0, reserves[in_index].symbol);
    if (mitr->lendables[in_index]) {
      pizzalend::pztoken pz = pizzalend::get_pztoken_byanchor(sym);
      double pzprice = pz.cal_pzprice();
      st_reserves[in_index] = pz.cal_anchor_quantity(reserves[in_index], pzprice);
      incr = pz.cal_pzquantity(st_incr, pzprice);
      _transfer_out(LEND_CONTRACT, contract, from_quantity, "collateral");
    } else {
      st_reserves[in_index] = reserves[in_index];
      incr = st_incr;
    }

    double x = asset2double(st_reserves[in_index]);
    reserves[in_index] += incr;
    st_reserves[in_index] += st_incr;

    pizzalend::pztoken out_pz;
    double out_pzprice = 0;
    if (mitr->lendables[out_index]) {
      out_pz = pizzalend::get_pztoken_byanchor(mitr->syms[out_index]);
      out_pzprice = out_pz.cal_pzprice();

      st_reserves[out_index] = out_pz.cal_anchor_quantity(reserves[out_index], out_pzprice);
    } else {
      st_reserves[out_index] = reserves[out_index];
    }
    double y = asset2double(st_reserves[out_index]);
    
    q = p_to_q(p, A, x, y);
    asset to_quantity = asset(q * pow(10, mitr->syms[out_index].get_symbol().precision()), mitr->syms[out_index].get_symbol());

    if (slippage > 0 && expect > 0 && to_quantity.amount < expect) {
      uint64_t min_got = expect * (1 - (double)slippage / 10000);
      print_f("min got: %, got: %, slippage protection: % ", min_got, to_quantity, slippage);
      check(to_quantity.amount >= min_got, "the slippage of this trade is too high");
    }

    if (fee_conf.index == out_index) {
      fee.amount = (double)to_quantity.amount * fee_rate;
      admin_fee.amount = (double)fee.amount * (1 - decimal2double(fee_conf.lp_rate));
      invite_fee.amount = (double)to_quantity.amount * invite_fee_rate;
      to_quantity -= fee;
    }

    if (fee_rate > 0) {
      check(admin_fee.amount > 0, "swap amount is too small");
    }

    print_f("pay: %, got: %, fee: % ", from_quantity, to_quantity, fee);
    _log_swap(account, mitr->lptoken.code(), from_quantity, to_quantity, fee);

    asset st_decr = to_quantity;
    if (fee_conf.index == out_index) {
      st_decr += admin_fee;
    }
    check(st_reserves[out_index] >= st_decr, "insufficient reserve");

    asset decr = asset(0, reserves[out_index].symbol);
    if (mitr->lendables[out_index]) {
      action(
        permission_level{_self, name("active")},
        LEND_CONTRACT,
        name("withdraw"),
        std::make_tuple(_self, mitr->syms[out_index].get_contract(), st_decr)
      ).send();
      decr = out_pz.cal_pzquantity(st_decr, out_pzprice);
      check(reserves[out_index] >= decr, "insufficient reserve");
    } else {
      decr = st_decr;
    }

    reserves[out_index] -= decr;
    st_reserves[out_index] -= st_decr;

    _transfer_out(account, mitr->syms[out_index].get_contract(), to_quantity, "swap");

    if (invite_fee.amount > 0 && ivt.is_valid()) {
      if (invite_fee > admin_fee) invite_fee = admin_fee;
      _transfer_out(ivt.account, mitr->syms[fee_conf.index].get_contract(), invite_fee, "invite rebate");
      admin_fee -= invite_fee;
    }

    if (admin_fee.amount > 0) {
      _transfer_out(PLANB_CONTRACT, mitr->syms[fee_conf.index].get_contract(), admin_fee, "admin fee");
    }

    _update_market_reserve(mitr, st_reserves, reserves, mitr->lpamount);
  };

  void pizzair::_on_lptoken_transfer(name from, name to, asset quantity, std::string memo) {
    symbol_code lpsym = quantity.symbol.code();
    auto mitr = markets.find(lpsym.raw());
    check(mitr != markets.end() && mitr->lptoken == quantity.symbol, "market not found");
    check(mitr->lpamount >= quantity.amount, "insufficient lpamount");

    auto liqdts_byacclpsym = liqdts.get_index<name("byacclpsym")>();
    auto itr = liqdts_byacclpsym.find(raw(from.value, lpsym.raw()));
    check(itr != liqdts_byacclpsym.end() && itr->lpamount >= quantity.amount, "insufficient liqdt lpamount");

    double ratio = (double)quantity.amount / itr->lpamount;
    std::vector<asset> gots = {asset(0, itr->reserves[0].symbol), asset(0, itr->reserves[1].symbol)};
    gots[0].amount = itr->reserves[0].amount * ratio;
    gots[1].amount = itr->reserves[1].amount * ratio;

    _incr_liqdt(to, mitr, gots, quantity.amount);
    
    _decr_liqdt(from, mitr, quantity.amount);
  };

  void pizzair::_demand(name account, name contract, asset quantity, int sym_index) {
    _check_allow(account, FEATURE_DEMAND);

    check(contract == LPTOKEN_CONTRACT, "only lptoken can demand");

    symbol_code lpsym = quantity.symbol.code();
    auto mitr = markets.find(lpsym.raw());
    check(mitr != markets.end() && mitr->lptoken == quantity.symbol, "market not found");
    check(mitr->lpamount >= quantity.amount, "insufficient lpamount");

    double ratio = (double)quantity.amount / mitr->lpamount;

    std::vector<asset> reserves = mitr->reserves;

    std::vector<asset> st_reserves = {asset(0, mitr->syms[0].get_symbol()), asset(0, mitr->syms[1].get_symbol())};

    std::vector<asset> gots;
    for (int i = 0; i <= 1; i++) {
      int64_t amount = mitr->reserves[i].amount * ratio;
      asset got = asset(amount, reserves[i].symbol);
      check(reserves[i] >= got, "insufficient reserve");
      reserves[i] -= got;

      if (mitr->lendables[i]) {
        pizzalend::pztoken pz = pizzalend::get_pztoken_byanchor(mitr->syms[i]);
        double pzprice = pz.cal_pzprice();

        st_reserves[i] = pz.cal_anchor_quantity(reserves[i], pzprice);
        if (got.amount > 0) {
          action(
            permission_level{_self, name("active")},
            LEND_CONTRACT,
            name("withdraw"),
            std::make_tuple(_self, pz.pzsymbol.get_contract(), got)
          ).send();
          got = pz.cal_anchor_quantity(got, pzprice);
        } else {
          got = asset(0, pz.anchor.get_symbol());
        }
      } else {
        st_reserves[i] = reserves[i];
      }
      gots.push_back(got);
    }

    _log_demand(account, lpsym, quantity, gots);

    _update_market_reserve(mitr, reserves, reserves, mitr->lpamount-quantity.amount);

    _decr_liqdt(account, mitr, quantity.amount);

    _retire_lptoken(quantity);
    
    int swap_index = -1;
    if (sym_index >= 0 && sym_index <= 1) {
      swap_index = sym_index == 0 ? 1 : 0;
    }

    for (int i = 0; i <= 1; i++) {
      if (gots[i].amount > 0) {
        if (swap_index == i) {
          _swap(mitr->lptoken.code(), account, mitr->syms[i].get_contract(), gots[i]);
        } else {
          _transfer_out(account, mitr->syms[i].get_contract(), gots[i], "demand");
        }
      }
    }
  };

  void pizzair::_transfer_out(name to, name contract, asset quantity, std::string memo) {
    action(
      permission_level{_self, name("active")},
      contract,
      name("transfer"),
      std::make_tuple(_self, to, quantity, memo)
    ).send();
  };

  void pizzair::_update_market_reserve(market_tlb::const_iterator mitr, std::vector<asset> st_reserves, std::vector<asset> reserves, uint64_t lpamount) {
    double r0 = asset2double(st_reserves[0]);
    double r1 = asset2double(st_reserves[1]);
    double p = std::min(r0, r1) * pow(10, -6);
    double price0 = 0;
    double price1 = 0;
    if (p > 0) {
      double A = _get_exact_leverage(mitr);
      price0 = p_to_q(p, A, r0, r1) / p;
      price1 = p_to_q(p, A, r1, r0) / p;
    }

    markets.modify(mitr, _self, [&](auto& row) {
      row.reserves[0] = reserves[0];
      row.reserves[1] = reserves[1];
      row.prices[0] = price0;
      row.prices[1] = price1;
      row.lpamount = lpamount;
    });

    _log_upmarket(mitr->lptoken.code(), st_reserves, mitr->prices, mitr->lpamount);
  };

  void pizzair::addpool(symbol_code psym, uint8_t decimals) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    auto exist = pools.find(psym.raw());
    check(exist == pools.end(), "pool with this sym already exists");

    pools.emplace(_self, [&](auto& row) {
      row.psym = psym;
      row.decimals = decimals;
    });
  };

  void pizzair::addmarket(symbol_code psym, extended_symbol sym0, extended_symbol sym1, market_config config) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    pool p = pools.get(psym.raw(), "pool not found");
    auto itr = _find_market(sym0, sym1);
    check(itr == markets.end(), "market already exists");

    symbol sym = _next_lptoken(p);
    _create_lptoken(asset(LPSYM_MAX_SUPPLY * pow(10, sym.precision()), sym));
    
    markets.emplace(_self, [&](auto& row) {
      row.lptoken = sym;
      row.syms = {sym0, sym1};
      row.reserves = {asset(0, sym0.get_symbol()), asset(0, sym1.get_symbol())};
      row.prices = {0, 0};
      row.lendables = {0, 0};
      row.lpamount = 0;
      row.config = config;
    });
  };

  void pizzair::setmarket(symbol_code lpsym, market_config config) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    auto itr = markets.find(lpsym.raw());
    check(itr != markets.end(), "market not found");
    markets.modify(itr, _self, [&](auto& row) {
      row.config = config;
    });
  };

  void pizzair::setlendable(symbol_code lpsym, extended_symbol sym, bool lendable) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    if (lpsym != ANY_LPSYM) {
      auto mitr = markets.require_find(lpsym.raw(), "market not found");
      int index = -1;
      for (auto i = 0; i < 2; i++) {
        if (mitr->syms[i] == sym) {
          index = i;
        }
      }
      check(index >= 0, "market does not match");
      return _setlendable(mitr, index, lendable);
    }

    for (auto itr = markets.begin(); itr != markets.end(); itr++) {
      for (auto i = 0; i < 2; i++) {
        if (itr->syms[i] == sym) {
          _setlendable(itr, i, lendable);
        }
      }
    }
  };

  void pizzair::setleverage(symbol_code lpsym, uint32_t leverage, uint32_t effective_secs) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    auto mitr = markets.require_find(lpsym.raw(), "market not found");
    uint32_t current_leverage = _get_leverage(mitr);

    auto litr = mleverages.find(lpsym.raw());
    if (litr == mleverages.end()) {
      mleverages.emplace(_self, [&](auto& row) {
        row.lptoken = mitr->lptoken;
        row.leverage = leverage;
        row.begined_at = current_secs();
        row.effective_secs = effective_secs;
      });
    } else {
      markets.modify(mitr, _self, [&](auto& row) {
        row.config.leverage = current_leverage;
      });

      mleverages.modify(litr, _self, [&](auto& row) {
        row.leverage = leverage;
        row.begined_at = current_secs();
        row.effective_secs = effective_secs;
      });
    }
  };

  void pizzair::setfee(symbol_code lpsym, decimal lp_rate, int index) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    auto mitr = markets.require_find(lpsym.raw(), "market not found");

    check(index >= 0 && index < mitr->syms.size(), "index out of range");
    double rate = decimal2double(lp_rate);
    check(rate >= 0 && rate <= 100, "lp rate out of range");

    auto itr = mfees.find(lpsym.raw());
    if (itr == mfees.end()) {
      mfees.emplace(_self, [&](auto& row) {
        row.lptoken = mitr->lptoken;
        row.lp_rate = lp_rate;
        row.index = index;
      });
    } else {
      mfees.modify(itr, _self, [&](auto& row) {
        row.lp_rate = lp_rate;
        row.index = index;
      });
    }
  };

  void pizzair::addallow(name account, name feature, uint32_t duration) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    _addto_allowlist(account, feature, AllowType::ManualAllow, duration);
  };

  void pizzair::addallows(std::vector<name> accounts, name feature, uint32_t duration) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    for (auto itr = accounts.begin(); itr != accounts.end(); itr++) {
      _addto_allowlist(*itr, feature, AllowType::ManualAllow, duration);
    }
  };

  void pizzair::remallow(name account, name feature) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    allowlist_tlb allows(_self, feature.value);
    auto itr = allows.find(account.value);
    check(itr != allows.end(), "the account is not in the allowlist");
    allows.erase(itr);
  };

  void pizzair::setinvite(name code, name account, decimal fee_rate) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    check(is_account(account), "invalid account");
    check(decimal2double(fee_rate) <= 0.0005, "the fee rate is too big");

    auto itr = invitations.find(code.value);
    if (itr == invitations.end()) {
      invitations.emplace(_self, [&](auto& row) {
        row.code = code;
        row.account = account;
        row.fee_rate = fee_rate;
      });
    } else {
      invitations.modify(itr, _self, [&](auto& row) {
        row.account = account;
        row.fee_rate = fee_rate;
      });
    }
  };

  void pizzair::setminsupply(symbol_code psym, uint64_t amount) {
    require_auth(permission_level{ADMIN_ACCOUNT, name("manager")});

    auto itr = minsupplies.find(psym.raw());
    if (itr == minsupplies.end()) {
      minsupplies.emplace(_self, [&](auto& row) {
        row.psym = psym;
        row.amount = amount;
      });
    } else {
      minsupplies.modify(itr, _self, [&](auto& row) {
        row.amount = amount;
      });
    }
  };

  void pizzair::_setlendable(market_tlb::const_iterator mitr, int index, bool lendable) {
    bool ori_lendable = mitr->lendables[index];
    if (ori_lendable == lendable) return;

    extended_symbol sym = mitr->syms[index];
    pizzalend::pztoken pz = pizzalend::get_pztoken_byanchor(sym);
    double pzprice = pz.cal_pzprice();
    
    if (lendable) {
      asset quantity = mitr->reserves[index];
      asset pzquantity = asset(0, pz.pzsymbol.get_symbol());
      if (quantity.amount > 0) {
        pzquantity = pz.cal_pzquantity(quantity, pzprice);

        _transfer_out(LEND_CONTRACT, sym.get_contract(), quantity, "collateral");
      }

      markets.modify(mitr, _self, [&](auto& row) {
        row.reserves[index] = pzquantity;
        row.lendables[index] = lendable;
      });
    } else {
      asset pzquantity = mitr->reserves[index];
      asset quantity = asset(0, mitr->syms[index].get_symbol());
      if (pzquantity.amount > 0) {
        quantity = pz.cal_anchor_quantity(pzquantity, pzprice);

        action(
          permission_level{_self, name("active")},
          LEND_CONTRACT,
          name("withdraw"),
          std::make_tuple(_self, pz.pzsymbol.get_contract(), pzquantity)
        ).send();
      }

      markets.modify(mitr, _self, [&](auto& row) {
        row.reserves[index] = quantity;
        row.lendables[index] = lendable;
      });
    }
  };

  symbol pizzair::_next_lptoken(pool p) {
    auto markets_bypsym = markets.get_index<name("bypsym")>();
    uint64_t next_id = 1;
    auto itr = markets_bypsym.lower_bound(p.psym.raw());

    int last_num = 0;
    while (itr != markets_bypsym.end() && itr->psym() == p.psym) {
      std::string roman = itr->lptoken.code().to_string().substr(p.psym.to_string().size());
      int num = roman_to_int(roman);
      if (num > last_num) {
        last_num = num;
      }
      itr++;
    }
    next_id = last_num + 1;
    std::string next_roman = int_to_roman(next_id);
    symbol_code next_code = symbol_code(p.psym.to_string() + next_roman);
    return symbol(next_code, p.decimals);
  };

  

  #ifndef MAINNET
    void pizzair::clear() {
      require_auth(_self);
      auto pitr = pools.begin();
      while (pitr != pools.end()) pitr = pools.erase(pitr);
      
      auto mitr = markets.begin();
      while (mitr != markets.end()) mitr = markets.erase(mitr);

      auto litr = liqdts.begin();
      while (litr != liqdts.end()) litr = liqdts.erase(litr);
    };
  #endif
}