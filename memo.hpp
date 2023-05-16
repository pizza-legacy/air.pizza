#pragma once

#include "common.hpp"

class memo {
  private:
    std::vector<std::string> ss;

  public:
    memo(const std::string& s) {
      ss = split_string(s, '-');
    }

    int len() const {
      return ss.size();
    }

    std::string get(int i) const {
      int l = len();
      if (i < 0) {
        if (i < -l) return "";
        i = l + i;
      }

      if (i >= l) return "";
      return ss[i];
    }
};