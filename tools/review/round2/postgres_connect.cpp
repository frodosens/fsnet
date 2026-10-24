#include "fsnet/database.hpp"
#include <iostream>
int main() {
 try { (void)fsnet::postgres_database("sslmode=disable"); }
 catch(const std::exception& e) { std::cout << "rejected=" << e.what() << '\n'; return 0; }
 return 2;
}
