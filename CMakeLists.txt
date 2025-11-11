cmake_minimum_required(VERSION 3.16)
project(P2P-network LANGUAGES CXX)

# C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Exécutable serveur
add_executable(node_chat src/node_chat.cpp)

# 👉 1) Inclure Asio (standalone)
target_include_directories(node_chat PRIVATE ${CMAKE_SOURCE_DIR}/external/asio-master/include)

# 👉 1.2) Inclure les fichier de /src --> notamment protocol.hpp
target_include_directories(node_chat PRIVATE ${CMAKE_SOURCE_DIR}/src)


# 👉 2) Dire à Asio qu'on est en standalone (sans Boost)
target_compile_definitions(node_chat PRIVATE ASIO_STANDALONE)


# 👉 3) Avertissements utiles (déjà chez toi)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(node_chat PRIVATE -Wall -Wextra -Wpedantic)
endif()


# 👉 4) (Optionnel mais recommandé) lier les threads
find_package(Threads REQUIRED)
target_link_libraries(node_chat PRIVATE Threads::Threads)