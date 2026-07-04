#include "net.hpp"

#include <iostream>
#include <string>
#include <thread> // Отдельный поток приёма клиентов и сообщений
#include <atomic> // Классы и функции для выполнения атомарных операций. Флаг завершения программы
#include <mutex> // Защита общих данных ClientRegistry
#include <vector>
#include <algorithm> // std::remove_if при удалении клиента из списка
#include <sstream> // Разбор команд, введённых в консоль хоста

// Удобное представление типа сообщения для логов
static std::string message_type_to_string(MessageType t) {
    switch (t) {
        case MessageType::Hello:     return "Hello";
        case MessageType::Welcome:   return "Welcome";
        case MessageType::Text:      return "Text";
        case MessageType::Track:     return "Track";
        case MessageType::Play:      return "Play";
        case MessageType::Pause:     return "Pause";
        case MessageType::Stop:      return "Stop";
        case MessageType::Volume:    return "Volume";
        case MessageType::Ping:      return "Ping";
        case MessageType::Pong:      return "Pong";
        case MessageType::Shutdown:  return "Shutdown";
        case MessageType::StateSync:  return "StateSync";
        default:                     return "Unknown";
    }
}

// Печать локальных IP-адресов
// Это нужно, чтобы пользователь увидел адреса для подключения по LAN
static void print_host_ips() {
    auto ips = get_local_ipv4_addresses();

    std::cout << "Available host IPs:\n";
    for (const auto& ip : ips) {
        std::cout << "  " << ip << '\n';
    }
    std::cout << std::endl;
}

// Список клиентов на сервере
// Отвечает за регистрацию, удаление и рассылку сообщений всем сразу
class ClientRegistry {
public:
    void add(int id, SOCKET sock) {
        std::lock_guard<std::mutex> lock(m_);
        clients_.push_back({id, sock});
    }

    void remove(SOCKET sock) {
        std::lock_guard<std::mutex> lock(m_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [&](const ClientEntry& e) { return e.sock == sock; }),
            clients_.end()
        );
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(m_);
        return clients_.size();
    }

    // Рассылка сообщения всем клиентам
    // Если у кого-то отправка не удалась, потом удалим его из списка
    void broadcast(const Message& msg) {
        std::vector<SOCKET> dead;

        {
            std::lock_guard<std::mutex> lock(m_);
            for (const auto& c : clients_) {
                if (!send_message(c.sock, msg)) {
                    dead.push_back(c.sock);
                }
            }
        }

        for (SOCKET s : dead) {
            remove(s);
            closesocket(s);
        }
    }

private:
    struct ClientEntry {
        int id;
        SOCKET sock;
    };

    mutable std::mutex m_;
    std::vector<ClientEntry> clients_;
};

// Состояние текущей сессии на сервере
// В нём хранится то, что нужно синхронизировать между клиентами
struct SessionState {
    std::string current_track = "music/test.mp3";
    uint64_t position_ms = 0;
    uint32_t volume = 100;

    // Статус проигрывания
    // playing / paused / stopped
    std::string state = "stopped";

    // Когда был отправлен текущий play
    uint64_t server_time_ms = 0;

    // Когда клиенты должны начать играть
    uint64_t play_at_ms = 0;
};

// Формирование полного сообщения синхронизации
// Используется при подключении нового клиента или когда нужно обновить состояние
static Message make_state_sync_message(const SessionState& st, uint32_t sequence = 0) {
    Message msg;
    msg.type = MessageType::StateSync;
    msg.sequence = sequence;
    msg.payload = build_state_sync_payload(
        st.current_track,
        st.state,
        st.position_ms,
        st.server_time_ms,
        st.play_at_ms,
        st.volume
    );
    return msg;
}

// Обработка одного клиента на сервере
// Здесь происходит handshake и приём ответов
static void client_session(SOCKET sock, int client_id, ClientRegistry& registry, std::atomic<bool>& running) {
    try {
        // Ждём Hello.
        auto hello = recv_message(sock);
        if (!hello || hello->type != MessageType::Hello) {
            closesocket(sock);
            return;
        }

        // Разбираем приветствие клиента.
        BinaryReader reader(hello->payload);
        std::string nickname = reader.read_string();
        uint64_t client_time = reader.read_u64();

        std::cout << "[host] client #" << client_id
                  << " hello from " << nickname
                  << ", client_time=" << client_time << "\n";

        // Клиент регистрируется только после успешного handshake
        registry.add(client_id, sock);

        // Отправляем приветствие от сервера
        Message welcome;
        welcome.type = MessageType::Welcome;
        welcome.sequence = 1;
        welcome.payload = build_welcome_payload(static_cast<uint32_t>(client_id), "sync_music_host");
        send_message(sock, welcome);

        // После Welcome полезно сразу прислать состояние сессии
        // Здесь клиент сможет увидеть текущий трек и статус
        SessionState initial_state;
        initial_state.current_track = "music/test.mp3";
        initial_state.state = "stopped";
        initial_state.position_ms = 0;
        initial_state.volume = 100;

        Message sync = make_state_sync_message(initial_state, 2);
        send_message(sock, sync);

        // Пока соединение живо, читаем сообщения от клиента
        while (running) {
            auto msg = recv_message(sock);
            if (!msg) {
                break;
            }

            switch (msg->type) {
                case MessageType::Pong: {
                    // В payload лежит timestamp, который клиент получил в Ping
                    uint64_t sent_at = parse_ping_payload(msg->payload);
                    uint64_t rtt = now_ms() - sent_at;
                    std::cout << "[host] client #" << client_id
                              << " pong, RTT ~ " << rtt << " ms\n";
                    break;
                }

                default:
                    std::cout << "[host] client #" << client_id
                              << " sent " << message_type_to_string(msg->type) << "\n";
                    break;
            }
        }
    }
    catch (const std::exception& e) {
        std::cout << "[host] client #" << client_id
                  << " session error: " << e.what() << "\n";
    }

    // Удаляем клиента и закрываем сокет
    registry.remove(sock);
    closesocket(sock);

    std::cout << "[host] client #" << client_id << " disconnected\n";
}

// Запуск сервера
static void run_host(uint16_t port) {
    SocketInit init;
    TcpServer server(port);
    server.start();

    std::cout << "[host] server started on port " << port << "\n";
    print_host_ips();

    ClientRegistry registry;
    std::atomic<bool> running{true};

    // Поток, который принимает новых клиентов
    std::thread accept_thread([&] {
        int next_client_id = 1;

        while (running) {
            try {
                SOCKET client = server.accept_client();

                // Ограничение по заданию: не больше 4 пользователей
                if (registry.count() >= 4) {
                    Message full_msg;
                    full_msg.type = MessageType::Text;
                    full_msg.sequence = 0;
                    full_msg.payload = build_text_payload("Server is full (max 4 clients).");
                    send_message(client, full_msg);
                    closesocket(client);
                    continue;
                }

                int client_id = next_client_id++;
                std::thread(client_session, client, client_id, std::ref(registry), std::ref(running)).detach();
            }
            catch (...) {
                // Если сервер уже выключается, исключение из accept() ожидаемо
                if (running) {
                    std::cout << "[host] accept loop error\n";
                }
            }
        }
    });

    std::cout << "[host] commands:\n";
    std::cout << "  text <message>\n";
    std::cout << "  track <name>\n";
    std::cout << "  play\n";
    std::cout << "  pause\n";
    std::cout << "  stop\n";
    std::cout << "  volume <0..100>\n";
    std::cout << "  ping\n";
    std::cout << "  quit\n\n";

    // Текущее состояние сессии
    SessionState session;
    session.current_track = "music/test.mp3";
    session.position_ms = 0;
    session.volume = 100;
    session.state = "stopped";

    // Консольный цикл управления хостом
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "quit") {
            // Перед выходом обязательно уведомляем клиентов
            Message shutdown_msg;
            shutdown_msg.type = MessageType::Shutdown;
            shutdown_msg.sequence = 100;
            shutdown_msg.payload = build_text_payload("Server is shutting down");
            registry.broadcast(shutdown_msg);
            break;
        }
        else if (cmd == "text") {
            std::string text;
            std::getline(iss >> std::ws, text);

            Message msg;
            msg.type = MessageType::Text;
            msg.sequence = 1;
            msg.payload = build_text_payload("[host text] " + text);
            registry.broadcast(msg);
        }
        else if (cmd == "track") {
            // Выбор нового трека
            std::string track;
            std::getline(iss >> std::ws, track);

            if (!track.empty()) {
                session.current_track = track;
                session.position_ms = 0;
                session.state = "stopped";

                Message msg;
                msg.type = MessageType::Track;
                msg.sequence = 2;
                msg.payload = build_track_payload(session.current_track);
                registry.broadcast(msg);

                // После выбора трека можно сразу прислать полную синхронизацию
                Message sync = make_state_sync_message(session, 3);
                registry.broadcast(sync);

                std::cout << "[host] current track set to: " << session.current_track << "\n";
            }
        }
        else if (cmd == "play") {
            // Запуск воспроизведения
            // Сервер задаёт момент, когда клиенты должны стартовать
            session.state = "playing";
            session.server_time_ms = now_ms();
            session.play_at_ms = session.server_time_ms + 150; // небольшая компенсация на сеть

            Message msg;
            msg.type = MessageType::Play;
            msg.sequence = 4;
            msg.payload = build_play_payload(
                session.current_track,
                session.position_ms,
                session.server_time_ms,
                session.play_at_ms,
                session.volume
            );
            registry.broadcast(msg);

            std::cout << "[host] PLAY broadcast: " << session.current_track << "\n";
        }
        else if (cmd == "pause") {
            // Пауза сохраняет позицию
            // Здесь мы не пересчитываем позицию по таймеру, это можно добавить позже
            session.state = "paused";
            session.server_time_ms = now_ms();

            Message msg;
            msg.type = MessageType::Pause;
            msg.sequence = 5;
            msg.payload = build_pause_payload(
                session.current_track,
                session.position_ms,
                session.server_time_ms
            );
            registry.broadcast(msg);

            std::cout << "[host] PAUSE broadcast\n";
        }
        else if (cmd == "stop") {
            // Полная остановка и сброс позиции
            session.state = "stopped";
            session.position_ms = 0;
            session.server_time_ms = now_ms();

            Message msg;
            msg.type = MessageType::Stop;
            msg.sequence = 6;
            msg.payload = build_stop_payload(
                session.current_track,
                session.server_time_ms
            );
            registry.broadcast(msg);

            std::cout << "[host] STOP broadcast\n";
        }
        else if (cmd == "volume") {
            // Уровень громкости в процентах
            int vol = 0;
            iss >> vol;

            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;

            session.volume = static_cast<uint32_t>(vol);

            Message msg;
            msg.type = MessageType::Volume;
            msg.sequence = 7;
            msg.payload = build_volume_payload(session.volume);
            registry.broadcast(msg);

            std::cout << "[host] VOLUME broadcast: " << vol << "%\n";
        }
        else if (cmd == "ping") {
            // Проверка задержки сети
            Message msg;
            msg.type = MessageType::Ping;
            msg.sequence = 8;
            msg.payload = build_ping_payload(now_ms());
            registry.broadcast(msg);

            std::cout << "[host] PING broadcast\n";
        }
        else {
            std::cout << "[host] unknown command\n";
        }
    }

    // Завершаем сервер
    running = false;
    server.stop();

    if (accept_thread.joinable()) {
        accept_thread.join();
    }

    std::cout << "[host] stopped\n";
}

// Запуск клиента.
static void run_client(const std::string& host_ip, uint16_t port) {
    SocketInit init;
    TcpClient client;
    client.connect_to(host_ip, port);

    std::cout << "[client] connected to " << host_ip << ":" << port << "\n";

    // Отправляем Hello сразу после подключения
    Message hello;
    hello.type = MessageType::Hello;
    hello.sequence = 1;
    hello.payload = build_hello_payload("client_one");
    send_message(client.socket_handle(), hello);

    std::atomic<bool> running{true};

    // Поток приёма сообщений от сервера
    std::thread rx([&] {
        try {
            while (running) {
                auto msg = recv_message(client.socket_handle());
                if (!msg) {
                    break;
                }

                switch (msg->type) {
                    case MessageType::Welcome: {
                        auto [client_id, server_name] = parse_welcome_payload(msg->payload);
                        std::cout << "[client] WELCOME from " << server_name
                                  << ", client id = " << client_id << "\n";
                        break;
                    }

                    case MessageType::Text: {
                        std::string text = parse_string_payload(msg->payload);
                        std::cout << "[client] TEXT: " << text << "\n";
                        break;
                    }

                    case MessageType::Track: {
                        std::string track = parse_string_payload(msg->payload);
                        std::cout << "[client] AUDIO placeholder: selected track -> " << track << "\n";
                        break;
                    }

                    case MessageType::StateSync: {
                        auto [track, state, pos, server_time, play_at, volume] =
                            parse_state_sync_payload(msg->payload);

                        std::cout << "[client] STATE SYNC: "
                                  << "track=" << track
                                  << ", state=" << state
                                  << ", pos=" << pos
                                  << ", server_time=" << server_time
                                  << ", play_at=" << play_at
                                  << ", volume=" << volume << "%\n";
                        break;
                    }

                    case MessageType::Play: {
                        auto [track, pos, server_time, play_at, volume] = parse_play_payload(msg->payload);
                        std::cout << "[client] AUDIO placeholder: PLAY '" << track
                                  << "' from " << pos << " ms"
                                  << ", server_time=" << server_time
                                  << ", play_at=" << play_at
                                  << ", volume=" << volume << "%\n";
                        break;
                    }

                    case MessageType::Pause: {
                        auto [track, pos, server_time] = parse_pause_payload(msg->payload);
                        std::cout << "[client] AUDIO placeholder: PAUSE '" << track
                                  << "' at " << pos << " ms"
                                  << ", server_time=" << server_time << "\n";
                        break;
                    }

                    case MessageType::Stop: {
                        auto [track, server_time] = parse_stop_payload(msg->payload);
                        std::cout << "[client] AUDIO placeholder: STOP '" << track
                                  << "', server_time=" << server_time << "\n";
                        break;
                    }

                    case MessageType::Volume: {
                        uint32_t vol = parse_volume_payload(msg->payload);
                        std::cout << "[client] AUDIO placeholder: VOLUME = " << vol << "%\n";
                        break;
                    }

                    case MessageType::Ping: {
                        // Отправляем обратно тот же timestamp, чтобы host смог оценить RTT
                        uint64_t t = parse_ping_payload(msg->payload);
                        Message pong;
                        pong.type = MessageType::Pong;
                        pong.sequence = msg->sequence;
                        pong.payload = build_ping_payload(t);
                        send_message(client.socket_handle(), pong);
                        break;
                    }

                    case MessageType::Shutdown: {
                        std::string text = parse_string_payload(msg->payload);
                        std::cout << "[client] SHUTDOWN: " << text << "\n";
                        running = false;
                        break;
                    }

                    default:
                        std::cout << "[client] unknown message\n";
                        break;
                }
            }
        }
        catch (const std::exception& e) {
            std::cout << "[client] receiver error: " << e.what() << "\n";
        }

        running = false;
    });

    std::cout << "[client] press Enter to disconnect\n";
    std::cin.get();

    running = false;

    // shutdown() нужен, чтобы recv() в потоке вышел и поток можно было завершить
    shutdown(client.socket_handle(), SD_BOTH);

    if (rx.joinable()) {
        rx.join();
    }

    std::cout << "[client] disconnected\n";
}

// Точка входа
// Первый аргумент -- режим: host или client
int main(int argc, char** argv) {
    try {
        std::cout << "sync-music-player " APP_VERSION "\n";

        if (argc < 2) {
            std::cout << "Usage:\n";
            std::cout << "  sync-music-player host [port]\n";
            std::cout << "  sync-music-player client <ip> [port]\n";
            return 0;
        }

        std::string mode = argv[1];
        uint16_t port = 54000;

        if (mode == "host") {
            if (argc >= 3) {
                port = static_cast<uint16_t>(std::stoi(argv[2]));
            }
            run_host(port);
        }
        else if (mode == "client") {
            if (argc < 3) {
                std::cerr << "Client mode requires IP address.\n";
                return 1;
            }

            std::string ip = argv[2];
            if (argc >= 4) {
                port = static_cast<uint16_t>(std::stoi(argv[3]));
            }

            run_client(ip, port);
        }
        else {
            std::cerr << "Unknown mode: " << mode << "\n";
            return 1;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}