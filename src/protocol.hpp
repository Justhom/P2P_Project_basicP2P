// ===========================================
// CLIENT_BROADCAST.CPP
// Client TCP synchrone (Asio standalone) :
// - Se connecte à 127.0.0.1:5555 (par défaut)
// - Thread lecture: affiche tout ce que le serveur envoie
// - Thread écriture: lit stdin, envoie chaque ligne avec '\n'
// ===========================================

#include <asio.hpp>               // Asio standalone
#include <iostream>                // std::cout, std::cerr
#include <thread>                  // std::thread
#include <string>                  // std::string

using asio::ip::tcp;

int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1";     // hôte par défaut
        std::string port = "5555";          // port par défaut
        if (argc > 1) host = argv[1];
        if (argc > 2) port = argv[2];

        asio::io_context io;                // contexte I/O
        tcp::resolver resolver(io);         // résolveur DNS/endpoint
        auto endpoints = resolver.resolve(host, port);

        tcp::socket socket(io);             // socket client
        asio::connect(socket, endpoints);   // connect synchrone

        std::cout << "[client] connecté à " << host << ":" << port << "\n";
        std::cout << "Tape des lignes et appuie sur Entrée. Ctrl+D (macOS/Linux) pour quitter.\n";

        // Thread de lecture: affiche tout ce qui arrive du serveur
        std::thread reader([&socket]() {
            try {
                for (;;) {
                    char data[1024];
                    std::error_code ec;
                    std::size_t n = socket.read_some(asio::buffer(data), ec);
                    if (ec) break;
                    std::cout.write(data, static_cast<std::streamsize>(n));
                    std::cout.flush();
                }
            } catch (...) {
                // sortie silencieuse
            }
        });

        // Boucle d’écriture: lit stdin et envoie chaque ligne
        std::string line;
        while (std::getline(std::cin, line)) {
            line.push_back('\n'); // assurer le '\n' pour le serveur
            asio::write(socket, asio::buffer(line));
        }

        // Fin propre: fermer le socket pour term. le reader
        std::error_code ignore;
        socket.shutdown(tcp::socket::shutdown_both, ignore);
        socket.close(ignore);

        if (reader.joinable()) reader.join();

    } catch (const std::exception& e) {
        std::cerr << "Erreur client: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
