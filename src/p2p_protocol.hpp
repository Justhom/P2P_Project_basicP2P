#pragma once
#define P2P_PROTOCOL_HEADER 1
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <optional>
#include <random>
#include <iomanip>
#include <deque>
#include <unordered_set>
#include <algorithm>

/*
   ===========================
   Protocole texte minimal v1
   ===========================
   - Une ligne = un message, terminé par '\n'
   - Séparateur de champs: '|'
   - Schémas supportés:
       1|JOIN|<node_id>|<host>|<port>\n
       1|PEERS|<n>|h1:p1|h2:p2|...\n
       1|CHAT|<msg_id>|<sender_id>|<payload...>\n
   - Remarque: le payload peut contenir des espaces (et même '|'
     si vous le voulez), mais pour rester simple, évitez '|' dans le texte.
*/

// --------------------
// Génération d'IDs hex
// --------------------
inline std::string random_hex(size_t bytes = 16) {
    // 16 octets -> 32 caractères hex → ID type UUID-like (sans tirets)
    std::random_device rd;
    std::mt19937_64 gen{rd()};
    std::uniform_int_distribution<unsigned> dist(0, 255);
    std::ostringstream oss;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned v = dist(gen);
        oss << std::hex << std::setw(2) << std::setfill('0') << (v & 0xff);
    }
    return oss.str();
}

inline std::string gen_node_id() { return random_hex(16); } // 128 bits
inline std::string gen_msg_id()  { return random_hex(16); } // 128 bits

// --------------------
// Split / Join helpers
// --------------------
inline std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(s.size());
    for (char c : s) {
        if (c == delim) {
            out.emplace_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.emplace_back(std::move(cur));
    return out;
}

inline std::string join(const std::vector<std::string>& fields, char delim) {
    std::ostringstream oss;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) oss << delim;
        oss << fields[i];
    }
    return oss.str();
}

// --------------------
// Types de messages
// --------------------
enum class MsgType { JOIN, PEERS, CHAT, PING, PONG, UNKNOWN };

inline MsgType parse_type(std::string_view s) {
    if (s == "JOIN")  return MsgType::JOIN;
    if (s == "PEERS") return MsgType::PEERS;
    if (s == "CHAT")  return MsgType::CHAT;
    if (s == "PING")  return MsgType::PING;
    if (s == "PONG")  return MsgType::PONG;
    return MsgType::UNKNOWN;
}

// Représentation "logique" après parse d'une ligne.
struct ParsedLine {
    int version = 0;         // attendu: 1
    MsgType type = MsgType::UNKNOWN;
    std::vector<std::string> fields; // champs spécifiques à TYPE (déjà sans "version" ni "TYPE")
};

// Parse une ligne brute "1|TYPE|f1|f2|...\n" (le '\n' peut être absent si déjà retiré).
inline std::optional<ParsedLine> parse_line(std::string_view raw_line) {
    // Retire un éventuel '\n' en fin de ligne
    if (!raw_line.empty() && raw_line.back() == '\n') {
        raw_line.remove_suffix(1);
    }

    auto parts = split(raw_line, '|');
    if (parts.size() < 2) return std::nullopt;

    // parts[0] = version, parts[1] = TYPE
    int ver = 0;
    try {
        ver = std::stoi(parts[0]);
    } catch (...) { return std::nullopt; }

    MsgType t = parse_type(parts[1]);

    std::vector<std::string> fields;
    fields.reserve(parts.size() > 2 ? parts.size() - 2 : 0);
    for (size_t i = 2; i < parts.size(); ++i) fields.push_back(std::move(parts[i]));

    return ParsedLine{ver, t, std::move(fields)};
}

// Encodeurs conviviaux (renvoient une ligne prête à envoyer, terminée par '\n')
inline std::string make_JOIN(std::string node_id, std::string host, uint16_t port) {
    std::vector<std::string> f = {
        "1", "JOIN", std::move(node_id), std::move(host), std::to_string(port)
    };
    return join(f, '|') + "\n";
}

inline std::string make_PEERS(const std::vector<std::string>& endpoints) {
    // endpoints sous forme "host:port"
    std::vector<std::string> f;
    f.reserve(3 + endpoints.size());
    f.push_back("1");
    f.push_back("PEERS");
    f.push_back(std::to_string(endpoints.size()));
    for (auto& ep : endpoints) f.push_back(ep);
    return join(f, '|') + "\n";
}

inline std::string make_CHAT(std::string msg_id, std::string sender_id, std::string payload) {
    std::vector<std::string> f = {
        "1", "CHAT", std::move(msg_id), std::move(sender_id), std::move(payload)
    };
    return join(f, '|') + "\n";
}

// -----------------------------
// LRU très simple pour msg_id
// -----------------------------
struct MsgIdLRU {
    // Maintient un ensemble de msg_id vus récemment pour éviter les boucles.
    // Impl ultra simple: deque FIFO bornée + set. Quand on dépasse la capacité,
    // on pop_front du deque et on efface du set.
    size_t capacity;
    std::deque<std::string> fifo;
    std::unordered_set<std::string> set;

    explicit MsgIdLRU(size_t cap = 1000) : capacity(cap) {}

    bool seen_or_insert(const std::string& id) {
        if (set.find(id) != set.end()) return true; // déjà vu
        // insérer
        fifo.push_back(id);
        set.insert(id);
        if (fifo.size() > capacity) {
            auto& old = fifo.front();
            set.erase(old);
            fifo.pop_front();
        }
        return false;
    }
};

// Petit helper: formate host:port
inline std::string to_hostport(const std::string& host, uint16_t port) {
    std::ostringstream oss;
    oss << host << ":" << port;
    return oss.str();
}
