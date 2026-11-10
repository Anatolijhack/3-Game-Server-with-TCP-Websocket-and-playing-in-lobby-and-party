#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "GameNet.h"
#include "PartyManager.h"
#include "Protocol.h"

// Серверный слой: сеть и маршрутизация. Правила пати живут в Party и PartyManager.
// Работает в игровом потоке, про сокеты ничего не знает.

constexpr std::uint32_t IDLE_TIMEOUT = 10 * TICK_RATE;
constexpr std::size_t   MAX_INPUT_QUEUE = 16;
constexpr std::size_t   MAX_CONNECTIONS = 64;               // защита от флуда
constexpr int           MAX_FAILS = 5;                      // неудачных входов
constexpr std::uint32_t FAIL_WINDOW = 60 * TICK_RATE;       // за минуту

struct InputCmd
{
    std::uint32_t seq = 0;
    std::uint8_t  flags = 0;
    std::uint8_t  nums = 0;
    std::int16_t  aimX = 0;
    std::int16_t  aimY = 0;
};

enum class ConnState { Menu, InParty };

struct Slot
{
    int           user_id = 0;
    std::string   name;
    ConnState     state = ConnState::Menu;
    std::deque<InputCmd> inputs;
    std::uint32_t last_seq = 0;
    std::uint32_t last_rx_tick = 0;
    bool          kicked = false;
    std::uint32_t fail_window_start = 0;
    int           fails = 0;
};

class GameHost : public IGameHandler
{
public:
    explicit GameHost(IGameNet& net) : net_(net) {}

    // ===== вызовы от сети =====

    void on_join(PlayerId id, int user_id, const std::string& name) override
    {
        if (slots_.size() >= MAX_CONNECTIONS || user_in_game(user_id))
        {
            net_.disconnect(id);
            return;
        }

        Slot s;
        s.user_id = user_id;
        s.name = name;
        s.last_rx_tick = tick_;
        s.fail_window_start = tick_;
        slots_[id] = std::move(s);

        // Welcome теперь значит только «вы вошли», мира у игрока ещё нет
        proto::Writer w;
        w.u8(proto::S2C_WELCOME);
        w.u32(id);
        w.u8(static_cast<std::uint8_t>(TICK_RATE));
        w.u16(static_cast<std::uint16_t>(ARENA_W));
        w.u16(static_cast<std::uint16_t>(ARENA_H));
        net_.send(id, w.b, true);
    }

    void on_disconnect(PlayerId id) override
    {
        leave_party(id, false, 0);
        slots_.erase(id);
    }

    void on_packet(PlayerId id, const std::uint8_t* data, std::size_t size) override
    {
        auto it = slots_.find(id);
        if (it == slots_.end() || size == 0) return;

        Slot& s = it->second;
        s.last_rx_tick = tick_;

        proto::Reader r(data, size);
        std::uint8_t type = 0;
        r.u8(type);

        switch (type)
        {
        case proto::C2S_INPUT:
        {
            InputCmd c;
            if (!r.u32(c.seq) || !r.u8(c.flags) || !r.u8(c.nums) || !r.i16(c.aimX) || !r.i16(c.aimY)
                || (c.flags & 0xC0) || (c.nums & 0xE0))
            {
                kick(id, s);
                return;
            }
            if (s.state != ConnState::InParty) break;     // в меню ввод игнорируем
            if (s.inputs.size() >= MAX_INPUT_QUEUE) s.inputs.pop_front();
            s.inputs.push_back(c);
            break;
        }

        case proto::C2S_PING:
        {
            proto::Writer w;
            w.u8(proto::S2C_PONG);
            net_.send(id, w.b, false);
            break;
        }

        case proto::C2S_CREATE_PARTY:
        {
            if (r.left() != 0) { kick(id, s); return; }
            if (s.state == ConnState::InParty) { send_error(id, proto::ERR_ALREADY_IN); break; }

            PErr err;
            Party* p = parties_.create(id, tick_, err);
            if (!p) { send_error(id, to_code(err)); break; }

            s.state = ConnState::InParty;
            s.inputs.clear();
            send_party_joined(id, *p);
            break;
        }

        case proto::C2S_JOIN_PARTY:
        {
            std::string code;
            if (!read_code(r, code)) { kick(id, s); return; }

            if (s.state == ConnState::InParty) { send_error(id, proto::ERR_ALREADY_IN); break; }
            if (!allow_attempt(s)) { send_error(id, proto::ERR_RATE); break; }

            PErr err;
            Party* p = parties_.join(id, code, tick_, err);
            if (!p)
            {
                ++s.fails;
                send_error(id, to_code(err));
                break;
            }

            s.state = ConnState::InParty;
            s.inputs.clear();
            send_party_joined(id, *p);
            send_party_update(*p, id);                    // остальным: новый состав
            break;
        }

        case proto::C2S_LEAVE_PARTY:
        {
            if (r.left() != 0) { kick(id, s); return; }
            if (s.state != ConnState::InParty) { send_error(id, proto::ERR_NOT_IN); break; }
            leave_party(id, true, proto::LEFT_BY_USER);
            break;
        }

        default:
            kick(id, s);                                   // неизвестный тип, в том числе повторный Hello
        }
    }

    // ===== один тик =====

    void tick()
    {
        ++tick_;

        // 1. Таймауты и ввод
        for (auto& [id, s] : slots_)
        {
            if (s.state != ConnState::InParty)
            {
                if (!s.kicked && tick_ - s.last_rx_tick > IDLE_TIMEOUT) kick(id, s);
                continue;
            }

            Party* p = parties_.of(id);
            if (!p) continue;
            const std::uint8_t num = p->num_of(id);

            if (s.kicked)
            {
                p->set_input(num, InputState{});          // кикнутый не бегает по старому вводу
                continue;
            }
            if (tick_ - s.last_rx_tick > IDLE_TIMEOUT)
            {
                kick(id, s);
                continue;
            }
            p->set_input(num, take_input(s));
        }

        // 2. Каждая пати тикает один раз и рассылает снимки своим участникам
        parties_.for_each([&](Party& p)
            {
                p.tick();

                proto::Writer world;
                p.write_world(world);

                for (const Member& m : p.members())
                {
                    auto it = slots_.find(m.conn);
                    if (it == slots_.end() || it->second.kicked) continue;

                    proto::Writer w;
                    w.u8(proto::S2C_SNAPSHOT);
                    w.u32(tick_);
                    w.u32(it->second.last_seq);
                    w.bytes(world.b);
                    net_.send(m.conn, w.b, false);
                }
            });

        if (tick_ % (10 * TICK_RATE) == 0) parties_.sweep();
    }

private:
    // ---------- пати ----------

    void leave_party(PlayerId id, bool notifySelf, std::uint8_t reason)
    {
        bool destroyed = false;
        Party* p = parties_.leave(id, destroyed);
        if (p) send_party_update(*p, 0);                  // остальным: состав и лидер

        auto it = slots_.find(id);
        if (it != slots_.end())
        {
            it->second.state = ConnState::Menu;
            it->second.inputs.clear();
        }
        if (notifySelf)
        {
            proto::Writer w;
            w.u8(proto::S2C_PARTY_LEFT);
            w.u8(reason);
            net_.send(id, w.b, true);
        }
    }

    void send_party_joined(PlayerId id, const Party& p)
    {
        proto::Writer w;
        w.u8(proto::S2C_PARTY_JOINED);
        w.u8(p.num_of(id));
        w.u8(p.leader());
        w.u8(static_cast<std::uint8_t>(p.members().size()));
        for (const Member& m : p.members()) w.u8(m.num);
        w.bytes(p.code().data(), p.code().size());
        net_.send(id, w.b, true);
    }

    // except: кому не слать (0 = слать всем)
    void send_party_update(const Party& p, PlayerId except)
    {
        proto::Writer w;
        w.u8(proto::S2C_PARTY_UPDATE);
        w.u8(p.leader());
        w.u8(static_cast<std::uint8_t>(p.members().size()));
        for (const Member& m : p.members()) w.u8(m.num);

        for (const Member& m : p.members())
            if (m.conn != except) net_.send(m.conn, w.b, true);
    }

    void send_error(PlayerId id, std::uint8_t code)
    {
        proto::Writer w;
        w.u8(proto::S2C_ERROR);
        w.u8(code);
        net_.send(id, w.b, true);
    }

    static std::uint8_t to_code(PErr e)
    {
        switch (e)
        {
        case PErr::AlreadyIn: return proto::ERR_ALREADY_IN;
        case PErr::Limit:     return proto::ERR_LIMIT;
        default:              return proto::ERR_NO_PARTY;   // NotFound и Full не различаем
        }
    }

    static bool read_code(proto::Reader& r, std::string& code)
    {
        char buf[proto::PARTY_CODE_LEN];
        if (!r.bytes(reinterpret_cast<std::uint8_t*>(buf), sizeof buf) || r.left() != 0)
            return false;
        code.assign(buf, sizeof buf);
        return true;
    }

    bool allow_attempt(Slot& s)
    {
        if (tick_ - s.fail_window_start > FAIL_WINDOW) { s.fail_window_start = tick_; s.fails = 0; }
        return s.fails < MAX_FAILS;
    }

    // ---------- ввод и соединения ----------

    InputState take_input(Slot& s)
    {
        InputState in{};
        if (s.inputs.empty()) return in;

        const InputCmd c = s.inputs.front();
        s.inputs.pop_front();

        in.left = (c.flags & proto::IN_LEFT) != 0;
        in.right = (c.flags & proto::IN_RIGHT) != 0;
        in.up = (c.flags & proto::IN_UP) != 0;
        in.down = (c.flags & proto::IN_DOWN) != 0;
        in.shoot = (c.flags & proto::IN_SHOOT) != 0;
        in.restart = (c.flags & proto::IN_RESTART) != 0;
        in.aimX = static_cast<float>(c.aimX);
        in.aimY = static_cast<float>(c.aimY);
        in.num1 = (c.nums & 1) != 0;
        in.num2 = (c.nums & 2) != 0;
        in.num3 = (c.nums & 4) != 0;
        in.num4 = (c.nums & 8) != 0;
        in.num5 = (c.nums & 16) != 0;

        s.last_seq = c.seq;
        return in;
    }

    void kick(PlayerId id, Slot& s)
    {
        if (s.kicked) return;
        s.kicked = true;
        net_.disconnect(id);             // отложенно: on_disconnect придёт позже
    }

    bool user_in_game(int user_id) const
    {
        for (const auto& kv : slots_)
            if (kv.second.user_id == user_id) return true;
        return false;
    }

    IGameNet& net_;
    std::unordered_map<PlayerId, Slot> slots_;
    PartyManager parties_;
    std::uint32_t tick_ = 0;
};