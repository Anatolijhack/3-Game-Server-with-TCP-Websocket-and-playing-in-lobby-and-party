// Party.h
#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "Instance.h"
#include "Gamenet.h"

struct Member
{
    std::uint8_t  num = 0;          // номер в пати 1..4, не мен€етс€, пока человек в пати
    PlayerId      conn = 0;         // текущее соединение (в 8e заменитс€)
    std::uint32_t joined_tick = 0;
};

class Party
{
public:
    explicit Party(std::string code)
        : code_(std::move(code)), world_(std::make_unique<Instance>()) {}

    const std::string& code() const { return code_; }
    bool empty() const { return members_.empty(); }
    bool full()  const { return members_.size() >= MAX_PARTY; }
    const std::vector<Member>& members() const { return members_; }

    // Ћидер = самый старый участник. —писок в пор€дке входа, поэтому это просто front().
    std::uint8_t leader() const { return members_.empty() ? 0 : members_.front().num; }

    // ¬озвращает номер участника или 0, если мест нет
    std::uint8_t add_member(PlayerId conn, std::uint32_t tick)
    {
        if (full()) return 0;

        const std::uint8_t num = free_num();
        if (num == 0) return 0;

        if (world_->add_player(num).isNull()) return 0;   // мира нет мест

        members_.push_back({ num, conn, tick });
        return num;
    }

    // true, если участник был
    bool remove_member(PlayerId conn)
    {
        for (auto it = members_.begin(); it != members_.end(); ++it)
        {
            if (it->conn == conn)
            {
                world_->remove_player(world_->get_player(it->num));
                members_.erase(it);      // лидер перейдЄт автоматически
                return true;
            }
        }
        return false;
    }

    std::uint8_t num_of(PlayerId conn) const
    {
        for (const auto& m : members_) if (m.conn == conn) return m.num;
        return 0;
    }

    void set_input(std::uint8_t num, const InputState& in)
    {
        world_->set_input(world_->get_player(num), in);
    }

    void tick() { world_->tick(); }
    void write_world(proto::Writer& w) const { world_->write_world(w); }

private:
    std::uint8_t free_num() const
    {
        for (std::uint8_t n = 1; n <= MAX_PARTY; ++n)
        {
            bool used = false;
            for (const auto& m : members_) if (m.num == n) { used = true; break; }
            if (!used) return n;
        }
        return 0;
    }

    std::string code_;
    std::vector<Member> members_;
    std::unique_ptr<Instance> world_;
};