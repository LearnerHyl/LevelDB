mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && cmake --build . && make
# cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
cmake -DCMAKE_BUILD_TYPE=Debug .. && cmake --build . && make