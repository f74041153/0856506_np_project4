all : socks_server.cpp console.cpp
	g++ socks_server.cpp -o socks_server -std=c++11 -Wall -pedantic -pthread -lboost_system
#	g++ console.cpp -o console.cgi -std=c++11 -Wall -pedantic -pthread -lboost_system
#main: main.cpp
#	g++ main.cpp -o main -std=c++11 -Wall -pedantic -pthread -lboost_system
