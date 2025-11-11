#include <asio.hpp>          // Asio standalone (header-only)
#include <iostream>
#include <memory>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

#include "p2p_protocol.hpp"      // helpers de protocole

#ifndef P2P_PROTOCOL_HEADER
# error "❌ Mauvais header: p2p_protocol.hpp non chargé ou header conflit."
#endif

using asio::ip::tcp;

// ============================
// Session = 1 connexion TCP
// ============================
// - Gère lecture continue par lignes (async_read_until '\n')
// - Gère une file d'écriture (deliver) pour sérialiser les async_write
// - Remonte les lignes reçues à Node via un callback
class Node; // forward

class Session : public std::enable_shared_from_this<Session> {
public:
    using OnLine = std::function<void(const std::string&, std::shared_ptr<Session>)>;

    Session(asio::io_context& io, OnLine on_line_cb)
        : socket_(io), on_line_(std::move(on_line_cb)) {}

    tcp::socket& socket() { return socket_; }

    void start() {
        // Démarre la boucle de lecture
        do_read_line();
    }

    // Envoie une ligne "brute" (déjà terminée par '\n') via la file d'écriture
    void deliver(const std::string& line) {
        bool write_in_progress = !outbox_.empty();
        outbox_.push_back(line);
        if (!write_in_progress) {
            do_write();
        }
    }

    // Pour debug / gestion: endpoint distant si dispo
    std::string remote_endpoint_str() const {
        std::error_code ec;
        auto ep = socket_.remote_endpoint(ec);
        if (ec) return "?";
        std::ostringstream oss;
        oss << ep.address().to_string() << ":" << ep.port();
        return oss.str();
    }

private:
    void do_read_line() {
        auto self = shared_from_this();
        asio::async_read_until(socket_, buffer_, '\n',
            [this, self](std::error_code ec, std::size_t /*n*/) {
                if (!ec) {
                    // Extraire une ligne complète du buffer
                    std::istream is(&buffer_);
                    std::string line;
                    std::getline(is, line); // retire le '\n'
                    line.push_back('\n');   // on rétablit le '\n' pour le traiter uniformément

                    // Remonter au Node (qui décidera quoi en faire)
                    if (on_line_) on_line_(line, self);

                    // Continuer à lire
                    do_read_line();
                } else {
                    // Erreur ou fermeture -> socket morte; on laisse Node nettoyer sa table
                    // (rien à faire ici; Node ne garde que des shared_ptr)
                }
            });
    }

    void do_write() {
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(outbox_.front()),
            [this, self](std::error_code ec, std::size_t /*n*/) {
                if (!ec) {
                    outbox_.pop_front();
                    if (!outbox_.empty()) {
                        do_write();
                    }
                } else {
                    // Échec d'écriture -> la session est probablement morte.
                    // On vide la file pour cesser les envois; Node enlèvera la session
                    outbox_.clear();
                }
            });
    }

    tcp::socket socket_;
    asio::streambuf buffer_;
    std::deque<std::string> outbox_;
    OnLine on_line_;
};

// ============================
// Node = accepte + connecte +
//        maintient les sessions
// ============================
class Node : public std::enable_shared_from_this<Node> {
public:
    Node(asio::io_context& io,
         std::string name,
         uint16_t listen_port,
         std::vector<std::pair<std::string, uint16_t>> bootstrap)
        : io_(io)
        , name_(std::move(name))
        , node_id_(gen_node_id())                // ID de nœud unique (aléatoire)
        , acceptor_(io, tcp::endpoint(tcp::v4(), listen_port))
        , listen_port_(listen_port)
        , bootstrap_(std::move(bootstrap))
        , seen_(1000) // LRU de 1000 messages
    {}

    void start() {
        std::cout << "[" << name_ << "] node_id=" << node_id_
                  << " listen=" << listen_port_ << "\n";

        start_accept();

        // Tente les connexions de bootstrap (si fournis en ligne de commande)
        for (auto& [h, p] : bootstrap_) {
            connect_to(h, p);
        }

        // Boucle d'input utilisateur (stdin) dans un thread séparé:
        // chaque ligne tapée est diffusée en 'CHAT'
        input_thread_ = std::thread([self=shared_from_this()] {
            std::string line;
            while (std::getline(std::cin, line)) {
                // Encapsule en CHAT (msg_id unique)
                std::string msg = make_CHAT(gen_msg_id(), self->node_id_, line);
                // Diffuse localement (affiche) + envoie aux pairs
                self->on_chat_local(msg);
            }
        });
    }

    ~Node() {
        if (input_thread_.joinable()) input_thread_.join();
    }

    // Callback principal: chaque Session appelle ceci quand une ligne arrive
    void on_line_received(const std::string& raw, std::shared_ptr<Session> src) {
        auto parsed = parse_line(raw);
        if (!parsed) return; // ligne invalide

        if (parsed->version != 1) return; // version non supportée

        switch (parsed->type) {
            case MsgType::JOIN:  handle_JOIN(*parsed, src);  break;
            case MsgType::PEERS: handle_PEERS(*parsed, src); break;
            case MsgType::CHAT:  handle_CHAT(*parsed, src, raw); break;
            case MsgType::PING:
            case MsgType::PONG:
            case MsgType::UNKNOWN:
            default: /* non utilisé dans v0 */ break;
        }
    }

    // Diffuse un message "CHAT" émis localement (par l'utilisateur)
    void on_chat_local(const std::string& chat_line) {
        // Affiche localement pour retour utilisateur
        auto p = parse_line(chat_line);
        if (p && p->type == MsgType::CHAT && p->fields.size() >= 3) {
            const std::string& payload = p->fields[2];
            std::cout << "[" << name_ << "] " << payload << "\n";
            // Dédup locale: marque comme vu avant d'envoyer (sinon renverra boucle locale si écho)
            if (p->fields.size() >= 1) {
                const std::string& msg_id = p->fields[0];
                seen_.seen_or_insert(msg_id);
            }
        }

        // Envoie aux pairs
        broadcast_except(chat_line, nullptr);
    }

private:
    // -------------------------
    // Acceptation des entrées
    // -------------------------
    void start_accept() {
        auto session = std::make_shared<Session>(io_,
            [this_weak = weak_from_this()](const std::string& l, std::shared_ptr<Session> s) {
                if (auto self = this_weak.lock()) self->on_line_received(l, s);
            });
        acceptor_.async_accept(session->socket(),
            [this, session](std::error_code ec) {
                if (!ec) {
                    add_session(session);
                    session->start();

                    // Dès qu'une session entrante s'ouvre, on lui envoie notre JOIN
                    auto host = local_host_guess();
                    session->deliver(make_JOIN(node_id_, host, listen_port_));

                    // Et on lui partage quelques endpoints connus (PEERS)
                    session->deliver(make_PEERS(current_endpoints()));
                }
                // Continuer à accepter
                start_accept();
            });
    }

    // -------------------------
    // Connexions sortantes
    // -------------------------
    void connect_to(const std::string& host, uint16_t port) {
        // Évite de se connecter à soi-même
        if ((host == "127.0.0.1" || host == "localhost") && port == listen_port_) return;

        tcp::resolver resolver(io_);
        auto results = resolver.resolve(host, std::to_string(port));
        auto session = std::make_shared<Session>(io_,
            [this_weak = weak_from_this()](const std::string& l, std::shared_ptr<Session> s) {
                if (auto self = this_weak.lock()) self->on_line_received(l, s);
            });

        asio::async_connect(session->socket(), results,
            [this, session, host, port](std::error_code ec, const tcp::endpoint& /*ep*/) {
                if (!ec) {
                    add_session(session);
                    session->start();

                    // À l'ouverture, on annonce notre présence (JOIN)
                    session->deliver(make_JOIN(node_id_, local_host_guess(), listen_port_));

                    // Puis on demande/partage la vue réseau (PEERS)
                    session->deliver(make_PEERS(current_endpoints()));
                } else {
                    std::cerr << "[" << name_ << "] connect_to " << host << ":" << port
                              << " failed: " << ec.message() << "\n";
                }
            });
    }

    // -------------------------
    // Handlers de messages
    // -------------------------
    void handle_JOIN(const ParsedLine& p, std::shared_ptr<Session> src) {
        // 1|JOIN|<node_id>|<host>|<port>
        if (p.fields.size() < 3) return;
        const std::string& nid  = p.fields[0];
        const std::string& host = p.fields[1];
        uint16_t port = 0;
        try { port = static_cast<uint16_t>(std::stoi(p.fields[2])); } catch (...) { return; }

        // Log simple
        std::cout << "[" << name_ << "] JOIN from " << nid
                  << " at " << host << ":" << port
                  << " via " << src->remote_endpoint_str() << "\n";

        // Ajoute ce pair à nos endpoints connus, et tente une connexion sortante
        // si nous ne l'avons pas déjà.
        // (cette redondance favorise la connectivité dans un petit graphe)
        add_known_endpoint(host, port);
        // Pour éviter de surconnecter, on pourrait vérifier si déjà connectés.
        maybe_connect_if_new(host, port);

        // Transmet le JOIN aux autres pairs (gossip/flood)
        relay_to_others(make_JOIN(nid, host, port), src);
    }

    void handle_PEERS(const ParsedLine& p, std::shared_ptr<Session> /*src*/) {
        // 1|PEERS|<n>|h1:p1|h2:p2|...
        if (p.fields.size() < 1) return;
        int n = 0;
        try { n = std::stoi(p.fields[0]); } catch (...) { return; }

        // Pour rester simple : tenter de se connecter aux endpoints listés
        for (int i = 0; i < n && (1+i) < (int)p.fields.size(); ++i) {
            auto& ep = p.fields[1+i];
            auto pos = ep.find(':');
            if (pos == std::string::npos) continue;
            std::string host = ep.substr(0, pos);
            uint16_t port = 0;
            try { port = static_cast<uint16_t>(std::stoi(ep.substr(pos+1))); } catch (...) { continue; }

            add_known_endpoint(host, port);
            maybe_connect_if_new(host, port);
        }
    }

    void handle_CHAT(const ParsedLine& p, std::shared_ptr<Session> src, const std::string& raw) {
        // 1|CHAT|<msg_id>|<sender_id>|<payload>
        if (p.fields.size() < 3) return;
        const std::string& msg_id = p.fields[0];
        const std::string& sender = p.fields[1];
        const std::string& payload = p.fields[2];

        // Déduplication: si déjà vu -> on ne relaie pas, on n'affiche pas
        if (seen_.seen_or_insert(msg_id)) return;

        // Affiche localement
        std::cout << "[" << sender << "] " << payload << "\n";

        // Relaye aux autres pairs (sauf la source)
        relay_to_others(raw, src);
    }

    // -------------------------
    // Aides de diffusion
    // -------------------------
    void relay_to_others(const std::string& line, std::shared_ptr<Session> except) {
        std::lock_guard<std::mutex> lg(mu_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (auto s = it->lock()) {
                if (s != except) s->deliver(line);
                ++it;
            } else {
                it = sessions_.erase(it); // nettoyage des sessions mortes
            }
        }
    }

    void broadcast_except(const std::string& line, std::shared_ptr<Session> except) {
        relay_to_others(line, except);
    }

    void add_session(const std::shared_ptr<Session>& s) {
        std::lock_guard<std::mutex> lg(mu_);
        sessions_.push_back(s);
        // Note: dans une version plus riche, on maintiendrait une map endpoint->session
    }

    // -------------------------
    // Gestion des endpoints connus
    // -------------------------
    void add_known_endpoint(const std::string& host, uint16_t port) {
        known_eps_.insert(to_hostport(host, port));
    }

    std::vector<std::string> current_endpoints() {
        // Retourne une petite liste d'endpoints connus pour PEERS
        std::vector<std::string> v;
        v.reserve(known_eps_.size());
        for (auto& s : known_eps_) v.push_back(s);
        // On pourrait limiter à N (ex: 16) si la liste est très grande
        return v;
    }

    void maybe_connect_if_new(const std::string& host, uint16_t port) {
        // Heuristique simple: tente une connexion si l'endpoint n'est pas "nous"
        // et si l'on a peu de connexions (pour éviter trop d'arêtes).
        if ((host == "127.0.0.1" || host == "localhost") && port == listen_port_) return;
        // Pour rester simple, on tente de se connecter (si déjà connecté, ce sera juste une duplication temporaire)
        connect_to(host, port);
    }

    // Déduction naïve de "notre" host à annoncer (pour usage local)
    std::string local_host_guess() const {
        // En local, 127.0.0.1 est suffisant pour démos multi-terminaux.
        // Sur LAN, remplace par IP locale si besoin.
        return "127.0.0.1";
    }

private:
    asio::io_context& io_;
    std::string name_;
    std::string node_id_;
    tcp::acceptor acceptor_;
    uint16_t listen_port_;
    std::vector<std::pair<std::string, uint16_t>> bootstrap_;

    // Sessions actives (weak_ptr pour permettre auto-nettoyage)
    std::vector<std::weak_ptr<Session>> sessions_;
    std::mutex mu_;

    // Ensemble d'endpoints "host:port" connus (pour PEERS)
    std::unordered_set<std::string> known_eps_;

    // Déduplication de messages
    MsgIdLRU seen_;

    // Thread de lecture clavier
    std::thread input_thread_;
};

// -------------------------
// Parsing CLI minimal
// -------------------------
struct Args {
    std::string name = "node";
    uint16_t listen = 0;
    std::vector<std::pair<std::string, uint16_t>> peers;
};

static std::optional<Args> parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--listen" && i+1 < argc) {
            a.listen = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (s == "--name" && i+1 < argc) {
            a.name = argv[++i];
        } else if (s == "--peer" && i+1 < argc) {
            std::string hp = argv[++i];
            auto pos = hp.find(':');
            if (pos == std::string::npos) return std::nullopt;
            std::string h = hp.substr(0, pos);
            uint16_t p = static_cast<uint16_t>(std::stoi(hp.substr(pos+1)));
            a.peers.emplace_back(std::move(h), p);
        } else {
            std::cerr << "Unknown arg: " << s << "\n";
            return std::nullopt;
        }
    }
    if (a.listen == 0) {
        std::cerr << "Usage: node_chat --listen <port> [--name X] [--peer host:port]...\n";
        return std::nullopt;
    }
    return a;
}

// -------------------------
// main()
// -------------------------
int main(int argc, char** argv) {
    auto args = parse_args(argc, argv);
    if (!args) return 1;

    try {
        asio::io_context io;

        auto node = std::make_shared<Node>(io, args->name, args->listen, args->peers);
        node->start();

        io.run(); // boucle Asio (réseau)

    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
