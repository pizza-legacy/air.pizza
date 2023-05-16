#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/symbol.hpp>
#include <eosio/system.hpp>
#include <libc/stdint.h>
#include <math.h>

using namespace eosio;

#define MAINNET

#define FLOAT symbol("F", 8)

typedef asset decimal;

decimal double2decimal(double a) {
  double tmp = a * pow(10, FLOAT.precision());
  return asset(int64_t(tmp), FLOAT);
};

double decimal2double(decimal d) {
  return (double)d.amount / pow(10, d.symbol.precision());
};

double asset2double(asset a) {
  return (double)a.amount / pow(10, a.symbol.precision());
};

decimal double2asset(double a, symbol sym) {
  double tmp = a * pow(10, sym.precision());
  return asset(int64_t(tmp), sym);
};

uint128_t raw(name n1, name n2) {
  return (uint128_t)n1.value << 64 | n2.value;
};

uint128_t raw(uint64_t i, uint64_t j) {
  return (uint128_t)i << 64 | j;
};

uint128_t raw(extended_symbol sym) {
  return (uint128_t)sym.get_contract().value << 64 | sym.get_symbol().raw();
};

std::vector<std::string> split_string(const std::string& s, const char delimiter = ' ') {
  auto last_pos = 0;
  auto pos = s.find_first_of(delimiter, last_pos);

  std::vector<std::string> ss;
  while(std::string::npos != pos || std::string::npos != last_pos) {
	  std::string sstr = pos > last_pos ? s.substr(last_pos, pos - last_pos) : "";
    ss.push_back(sstr);
	  if (std::string::npos == pos) break;
    last_pos = pos + 1;
    pos = s.find_first_of(delimiter, last_pos);
  }
  return ss;
};

std::string romans[16] = {"I", "II", "III", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII"};

int roman_to_int(std::string s) {
  for (int i = 0; i < 16; i++) {
    if (s == romans[i]) return i+1;
  }
  check(false, "unknown roman number");
  return 0;
};

std::string int_to_roman(int num) {
  check(num >= 1 && num <= 17, "invalid number");
  return romans[num - 1];
};

uint32_t current_secs() {
  time_point tp = current_time_point();
  return tp.sec_since_epoch();
};

uint64_t current_millis() {
  time_point tp = current_time_point();
  return tp.time_since_epoch().count()/1000;
};