/*
 * Define JSON11_TEST_CUSTOM_CONFIG to 1 if you want to build this tester into
 * your own unit-test framework rather than a stand-alone program.  By setting
 * The values of the variables included below, you can insert your own custom
 * code into this file as it builds, in order to make it into a test case for
 * your favorite framework.
 */
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <chrono>
#include <fstream>
#include <json11.hpp>

int main(int argc, char **argv) {
  std::string file_path;
  extern int memory_malloc_type;

  if (argc == 3) {
    file_path = argv[1];
    memory_malloc_type = atoi(argv[2]);
  } else {
    std::cerr << "Usage: " << argv[0] << " <file_path> <memory_malloc_type>" << std::endl;
    return 1;
  }

  std::cout << "Running " << argv[0] << std::endl;

  std::cout << "memory_malloc_type: " << memory_malloc_type << std::endl;

  std::ifstream file(file_path);
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

  std::chrono::time_point<std::chrono::steady_clock> start = std::chrono::steady_clock::now();

  std::string err_comment;
  json11::Json json = json11::Json::parse(content, err_comment, json11::JsonParse::STANDARD);

  std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
  std::chrono::duration<double> duration = end - start;
  std::cout << "Time: " << duration.count() << " seconds" << std::endl;

  std::cout << "done" << std::endl;

  return 0;
}
