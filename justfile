[linux]
build_client:
  g++ -g -Wall -o main_client -I src_railroad src_railroad/*.cpp src_client/*.cpp

[linux]
run_client: build_client
  ./main_client

[linux]
build_server:
  g++ -g -Wall -o main_server -I src_railroad src_railroad/*.cpp src_server/*.cpp

[linux]
run_server: build_server
  ./main_server


[windows]
build_client:
  g++ -g -Wall -o main_client.exe -I src_railroad src_railroad/*.cpp src_client/*.cpp

[windows]
run_client: build_client
  ./main_client.exe

[windows]
build_server:
  g++ -g -Wall -o main_server.exe -I src_railroad src_railroad/*.cpp src_server/*.cpp

[windows]
run_server: build_server
  ./main_server.exe