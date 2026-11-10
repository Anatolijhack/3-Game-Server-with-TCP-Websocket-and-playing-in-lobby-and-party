#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Кадр в сети:  [u32 длина, big-endian][данные]   (рамку делает Session)
// Данные:       [u8 тип][поля, little-endian]
// Предполагается little-endian процессор (x86, ARM).

namespace proto {

    // Должен совпадать с MAX_RAW_FRAME в Session.cpp
    constexpr std::size_t MAX_PACKET = 8192;

    // ---------- клиент -> сервер ----------
    constexpr std::uint8_t C2S_HELLO = 1;   // [токен, ASCII, до 128 байт] (обрабатывает GameGateway)
    constexpr std::uint8_t C2S_INPUT = 2;   // [u32 seq][u8 флаги][u8 клавиши 1-5][i16 aimX][i16 aimY]  // в меню сервер игнорирует
    constexpr std::uint8_t C2S_PING = 3;   // без полей


    constexpr std::uint8_t C2S_CREATE_PARTY = 4;   // без полей
    constexpr std::uint8_t C2S_JOIN_PARTY = 5;   // [6 байт код, ASCII]
    constexpr std::uint8_t C2S_LEAVE_PARTY = 6;   // без полей

    // Флаги ввода (поля left/right/up/down/shoot/restart вашего InputState)
    constexpr std::uint8_t IN_LEFT = 1;
    constexpr std::uint8_t IN_RIGHT = 2;
    constexpr std::uint8_t IN_UP = 4;
    constexpr std::uint8_t IN_DOWN = 8;
    constexpr std::uint8_t IN_SHOOT = 16;
    constexpr std::uint8_t IN_RESTART = 32;
    // Клавиши: бит 0..4 = num1..num5 (нажата ли клавиша прямо сейчас)

    // ---------- сервер -> клиент ----------
    constexpr std::uint8_t S2C_WELCOME = 1;     // [u32 id][u8 частота тика][u16 ширина][u16 высота]
    constexpr std::uint8_t S2C_SNAPSHOT = 2;
    constexpr std::uint8_t S2C_PONG = 3;
    constexpr std::uint8_t S2C_PARTY_JOINED = 4;   // [u8 ваш номер][u8 лидер][u8 n][n x u8 номер][6 байт код]
    constexpr std::uint8_t S2C_PARTY_UPDATE = 5;   // [u8 лидер][u8 n][n x u8 номер]
    constexpr std::uint8_t S2C_PARTY_LEFT = 6;   // [u8 причина]
    constexpr std::uint8_t S2C_ERROR = 7;   // [u8 код]

    constexpr std::size_t PARTY_CODE_LEN = 6;

    // Причины PARTY_LEFT
    constexpr std::uint8_t LEFT_BY_USER = 0;
    constexpr std::uint8_t LEFT_CLOSED = 1;
    constexpr std::uint8_t LEFT_KICKED = 2;

    // Коды ERROR
    constexpr std::uint8_t ERR_ALREADY_IN = 1;
    constexpr std::uint8_t ERR_NOT_IN = 2;
    constexpr std::uint8_t ERR_NO_PARTY = 3;   // «нет такой» и «заполнена» не различаем
    constexpr std::uint8_t ERR_LIMIT = 4;   // лимит пати на сервере
    constexpr std::uint8_t ERR_RATE = 5;
    constexpr std::uint8_t S2C_KICK = 0x7F;  // [u8 причина], см. KICK_* в GameNet.h

    // S2C_SNAPSHOT:
    //   [u32 тик][u32 ack_seq]                        заголовок (свой для каждого игрока)
    //   [u8 фаза][u8 бонус A][u8 бонус B][u8 секунд до конца выбора]
    //                                                 фаза: 0 игра, 1 выбор бонуса, 2 победа, 3 поражение
    //                                                 бонусы (номера UpgradeKind) и секунды имеют смысл только в фазе 1
    //   [u16 волна][u32 счёт]
    //   [u8 n] n x { u32 id, i16 x, i16 y, u8 hp, u8 max, u8 оружие(WeaponType), u8 флаги, u16 очки }
    //                                                 игроки пати; флаги: бит0 жив, бит1 бонус уже выбран
    //   [u16 n] n x { u32 id, u8 вид(EnemyKind), i16 x, i16 y, u16 hp, u16 max }   враги
    //   [u16 n] n x { u8 флаги, i16 x, i16 y }                                      пули; флаги: бит0 от игрока, бит1 ракета
    //   [u8 n]  n x { i16 x1, i16 y1, i16 x2, i16 y2 }                              лучи
    //   [u8 n]  n x { i16 x, i16 y, u16 радиус, u8 t }                              взрывы; t = life/maxLife * 255
    //   [u16 n] n x { i16 x, i16 y, u8 t }                                          огонь;  t = life/maxLife * 255
    //   [u8 n]  n x { i16 x, i16 y, i16 w, i16 h }                                  стены

    class Writer {
    public:
        void u8(std::uint8_t v) { b.push_back(v); }

        void u16(std::uint16_t v)
        {
            b.push_back(static_cast<std::uint8_t>(v & 0xFF));
            b.push_back(static_cast<std::uint8_t>(v >> 8));
        }

        void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }

        void u32(std::uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }

        void f32(float f)
        {
            std::uint32_t v;
            std::memcpy(&v, &f, 4);
            u32(v);
        }

        void bytes(const std::vector<std::uint8_t>& other) { b.insert(b.end(), other.begin(), other.end()); }

        void bytes(const void* p, std::size_t n)
        {
            const auto* s = static_cast<const std::uint8_t*>(p);
            b.insert(b.end(), s, s + n);
        }
        std::vector<std::uint8_t> b;
    };

    // Все чтения проверяют границы и возвращают false, если данных не хватило.
    class Reader {
    public:
        Reader(const std::uint8_t* d, std::size_t n) : p_(d), end_(d + n) {}

        bool bytes(std::uint8_t* out, std::size_t n)
        {
            if (left() < n) return false;
            std::memcpy(out, p_, n);
            p_ += n;
            return true;
        }

        bool u8(std::uint8_t& v)
        {
            if (p_ + 1 > end_) return false;
            v = *p_++;
            return true;
        }

        bool u16(std::uint16_t& v)
        {
            if (p_ + 2 > end_) return false;
            v = static_cast<std::uint16_t>(p_[0] | (p_[1] << 8));
            p_ += 2;
            return true;
        }

        bool i16(std::int16_t& v)
        {
            std::uint16_t t;
            if (!u16(t)) return false;
            v = static_cast<std::int16_t>(t);
            return true;
        }

        bool u32(std::uint32_t& v)
        {
            if (p_ + 4 > end_) return false;
            v = static_cast<std::uint32_t>(p_[0])
                | (static_cast<std::uint32_t>(p_[1]) << 8)
                | (static_cast<std::uint32_t>(p_[2]) << 16)
                | (static_cast<std::uint32_t>(p_[3]) << 24);
            p_ += 4;
            return true;
        }

        bool f32(float& f)
        {
            std::uint32_t v;
            if (!u32(v)) return false;
            std::memcpy(&f, &v, 4);
            return true;
        }

        std::size_t left() const { return static_cast<std::size_t>(end_ - p_); }
        const std::uint8_t* ptr() const { return p_; }

    private:
        const std::uint8_t* p_;
        const std::uint8_t* end_;
    };

} // namespace proto