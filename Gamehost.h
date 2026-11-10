#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "GameNet.h"
#include "Instance.h"
#include "Protocol.h"

// Серверный слой: каждому подключённому игроку свой мир (Instance) с вашей игрой.
// Живёт в игровом потоке и про сокеты ничего не знает. Назван GameHost, а файл GameHost.h,
// чтобы не пересекаться с вашим клиентским Game.h (GameState, UpgradeKind и т.д.).

constexpr std::uint32_t IDLE_TIMEOUT = 10 * TICK_RATE;   // тиков тишины до отключения
constexpr std::size_t   MAX_INPUT_QUEUE = 16;               // команд ввода в очереди на игрока

// У каждого Instance внутри ModuleRegistry свой пул на 4 потока, поэтому игроков пока немного.
// Если нужно больше, дайте ModuleRegistry общий пул (см. описание).
constexpr std::size_t   MAX_INSTANCES = 8;

struct InputCmd
{
    std::uint32_t seq = 0;
    std::uint8_t  flags = 0;
    std::uint8_t  nums = 0;
    std::int16_t  aimX = 0;
    std::int16_t  aimY = 0;
};

struct Slot
{
    std::unique_ptr<Instance> inst;
    int           user_id = 0;
    std::string   name;
    std::deque<InputCmd> inputs;
    std::uint32_t last_seq = 0;          // последняя обработанная команда (уходит как ack_seq)
    std::uint32_t last_rx_tick = 0;
    bool          kicked = false;
};

class GameHost : public IGameHandler
{
public:
    explicit GameHost(IGameNet& net) : net_(net) {}

    // ===== вызовы от сети (в игровом потоке) =====

    void on_join(PlayerId id, int user_id, const std::string& name) override
    {
        if (slots_.size() >= MAX_PARTY || user_in_game(user_id))
        {
            net_.disconnect(id);
            return;
        }

        if (!world_) world_ = std::make_unique<Instance>();

        Entity e = world_->add_player(static_cast<std::uint32_t>(id));
   
        Slot s;
        s.user_id = user_id;
        s.name = name;
        s.last_rx_tick = tick_;
        slots_[id] = std::move(s);

        world_->add_player(static_cast<std::uint32_t>(id));

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
        if (world_)
        {
            Entity e = world_->get_player(static_cast<std::uint32_t>(id));
            world_->remove_player(e);
        }

        slots_.erase(id);

        if (slots_.empty())
            world_.reset();
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
        default:
            kick(id, s);                 // неизвестный тип (в том числе повторный Hello): строго
        }
    }

    // ===== один тик (раз в 1/TICK_RATE секунды) =====

    void tick()
    {
        ++tick_;

        if (!world_)
            return;

        // 1. Принимаем ввод каждого игрока
        for (auto& [id, s] : slots_)
        {
            if (s.kicked)
                continue;

            if (tick_ - s.last_rx_tick > IDLE_TIMEOUT)
            {
                kick(id, s);
                continue;
            }

            InputState in = take_input(s);

            Entity e = world_->get_player(static_cast<std::uint32_t>(id));
            world_->set_input(e, in);
        }

        // 2. Обновляем общий игровой мир ОДИН раз
        world_->tick();

        // 3. Формируем общую часть Snapshot ОДИН раз
        proto::Writer worldData;
        world_->write_world(worldData);

        // 4. Отправляем каждому игроку его Snapshot
        for (auto& [id, s] : slots_)
        {
            if (s.kicked)
                continue;

            proto::Writer w;

            w.u8(proto::S2C_SNAPSHOT);
            w.u32(tick_);
            w.u32(s.last_seq);

            // Общие данные мира для всех игроков
            w.bytes(worldData.b);

            net_.send(id, w.b, false);
        }
    }

private:
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
    std::unique_ptr<Instance> world_;
    std::uint32_t tick_ = 0;
};