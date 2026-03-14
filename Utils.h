#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace util
{

  template <typename... Args>
  void print(Args &&...args)
  {
    (std::cout << ... << std::forward<Args>(args));
  }

  template <typename... Args>
  void printErr(Args &&...args)
  {
    (std::cerr << ... << std::forward<Args>(args));
  }

  inline uint64_t toUint64OrZero(const std::string &value)
  {
    if (value.empty())
    {
      return 0;
    }

    try
    {
      return std::stoull(value);
    }
    catch (...)
    {
      return 0;
    }
  }

  inline std::string trim(const std::string &value)
  {
    const std::string whitespace = " \t\r\n";
    const std::size_t start = value.find_first_not_of(whitespace);

    if (start == std::string::npos)
    {
      return "";
    }

    const std::size_t end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
  }

}
