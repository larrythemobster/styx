#pragma once

#include <nlohmann/json.hpp>

#include <concepts>
#include <charconv>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Flags.hpp"
#include "OrderedJson.hpp"
#include "Platform/Native.hpp"
#include "Snowflake.hpp"

namespace Styx::Core::JsonUtils {

namespace detail {
template <class> struct is_flags : std::false_type {};
template <class E> struct is_flags<Flags<E>> : std::true_type {};
template <class T> concept Sequence = !std::same_as<T, std::string> && requires(T t) {
    typename T::value_type;
    t.begin();
    t.end();
    t.push_back(std::declval<typename T::value_type>());
};
template <class T> concept Set = requires(T t) {
    typename T::value_type;
    t.begin();
    t.end();
    t.insert(std::declval<typename T::value_type>());
};
template <class T> concept Map = requires(T t) {
    typename T::key_type;
    typename T::mapped_type;
    t.begin();
    t.end();
    t.insert(std::declval<typename T::value_type>());
};
inline Snowflake snowflake(std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? Snowflake(value) : Snowflake::Invalid;
}
}

template <class T>
concept Serializable = requires(const T &t) {
    { t.toJson() } -> std::convertible_to<OrderedJson>;
};
template <class T>
concept Deserializable = requires(const nlohmann::json &value) {
    { T::fromJson(value) } -> std::convertible_to<T>;
};
template <class T>
concept FlagType = detail::is_flags<T>::value;

class JsonObject
{
public:
    template <class T> requires std::same_as<T, JsonObject>
    bool operator==(const T &) const { return true; }

    // Public so repositories can bind Field values; the JSON helpers below stay protected.
    template <class T, bool IsOptional = false, bool IsNullable = false>
    class Field
    {
    public:
        Field() { state = IsOptional ? State::Undefined : State::Value; }
        Field(const Field &) = default;
        Field(Field &&) = default;
        Field &operator=(const Field &) = default;
        Field &operator=(Field &&) = default;
        Field(const T &v) : state(State::Value), value(v) {}
        Field &operator=(const T &v) { value = v; state = State::Value; return *this; }
        Field &operator=(std::nullptr_t) { static_assert(IsNullable); state = State::Null; return *this; }
        Field(std::nullptr_t) { static_assert(IsNullable); state = State::Null; }

        template <class U> bool operator==(const U &other) const { return value == other; }
        bool operator==(const Field &other) const { return state == other.state && (state != State::Value || value == other.value); }

        [[nodiscard]] bool isUndefined() const { return IsOptional && state == State::Undefined; }
        [[nodiscard]] bool isNull() const { return IsNullable && state == State::Null; }
        [[nodiscard]] bool hasValue() const { return state == State::Value; }
        [[nodiscard]] T valueOr(const T &fallback = T()) const { return hasValue() ? value : fallback; }

        T &get() { return value; }
        const T &get() const { return value; }
        T &operator*() { return value; }
        const T &operator*() const { return value; }
        T *operator->() { return &value; }
        const T *operator->() const { return &value; }
        operator T &() { return value; }
        operator const T &() const { return value; }
        void undefine() { static_assert(IsOptional); state = State::Undefined; }

    private:
        enum class State { Undefined, Null, Value };
        State state = IsOptional ? State::Undefined : State::Value;
        T value{};
    };

protected:
    template <class T> static nlohmann::json toJsonValue(const T &value)
    {
        if constexpr (std::same_as<T, bool>) return value;
        else if constexpr (std::same_as<T, Snowflake>) return value.toString();
        else if constexpr (std::same_as<T, std::string>) return value;
        else if constexpr (std::same_as<T, Platform::DateTime>) return value.iso8601();
        else if constexpr (std::same_as<T, Platform::Color>) return value.argb();
        else if constexpr (std::same_as<T, Platform::Size>) return { value.width, value.height };
        else if constexpr (std::same_as<T, std::vector<uint8_t>>) return value;
        else if constexpr (std::unsigned_integral<T>) return static_cast<int64_t>(value);
        else if constexpr (FlagType<T>) return static_cast<int64_t>(value.toInt());
        else if constexpr (std::is_enum_v<T>) return static_cast<int64_t>(static_cast<std::underlying_type_t<T>>(value));
        else return nlohmann::json(value);
    }

    template <class T> static void insertValue(OrderedJson &object, std::string_view key, const T &value)
    {
        if constexpr (Serializable<T>) object.insert(key, value.toJson());
        else if constexpr (detail::Map<T>) {
            OrderedJson map;
            for (const auto &[k, v] : value) {
                if constexpr (std::same_as<typename T::key_type, Snowflake>)
                    insertValue(map, k.toString(), v);
                else
                    insertValue(map, k, v);
            }
            object.insert(key, map);
        } else if constexpr (std::same_as<T, std::vector<uint8_t>>) {
            OrderedJson::Array array;
            for (uint8_t byte : value) array.append(static_cast<int>(byte));
            object.insert(key, array);
        } else if constexpr (detail::Sequence<T> || detail::Set<T>) {
            OrderedJson::Array array;
            for (const auto &element : value) {
                if constexpr (Serializable<std::remove_cvref_t<decltype(element)>>) array.append(element.toJson());
                else array.append(toJsonValue(element));
            }
            object.insert(key, array);
        } else object.insert(key, toJsonValue(value));
    }

    template <class T, bool O, bool N>
    static void insert(OrderedJson &object, std::string_view key, const Field<T, O, N> &field)
    {
        if (field.isUndefined()) return;
        if (field.isNull()) { object.insert(key, nlohmann::json()); return; }
        insertValue(object, key, field.get());
    }

    template <class T> static T fromJsonValue(const nlohmann::json &value)
    {
        if constexpr (Deserializable<T>) return T::fromJson(value);
        else if constexpr (std::same_as<T, Snowflake>) return value.is_string() ? detail::snowflake(value.get_ref<const std::string &>()) : Snowflake(value.get<uint64_t>());
        else if constexpr (std::same_as<T, std::string>) return value.get<std::string>();
        else if constexpr (std::same_as<T, Platform::DateTime>) return Platform::DateTime::parse(value.get<std::string>());
        else if constexpr (std::same_as<T, Platform::Color>) return Platform::Color::parse(value.is_number() ? std::to_string(value.get<uint32_t>()) : value.get<std::string>());
        else if constexpr (std::same_as<T, Platform::Size>) return { value.at(0).get<int>(), value.at(1).get<int>() };
        else if constexpr (std::same_as<T, std::vector<uint8_t>>) {
            std::vector<uint8_t> bytes;
            if (value.is_array()) {
                bytes.reserve(value.size());
                for (const auto &item : value) bytes.push_back(item.get<int>());
            } else if (value.is_string()) {
                const auto text = value.get<std::string>();
                bytes.assign(text.begin(), text.end());
            }
            return bytes;
        } else if constexpr (FlagType<T>) return T::fromInt(static_cast<typename T::Int>(value.get<int64_t>()));
        else if constexpr (std::is_enum_v<T>) return static_cast<T>(value.get<int64_t>());
        else if constexpr (detail::Map<T>) {
            T map;
            for (auto it = value.begin(); it != value.end(); ++it) {
                if constexpr (std::same_as<typename T::key_type, Snowflake>)
                    map.emplace(detail::snowflake(it.key()), fromJsonValue<typename T::mapped_type>(it.value()));
                else
                    map.emplace(it.key(), fromJsonValue<typename T::mapped_type>(it.value()));
            }
            return map;
        } else if constexpr (detail::Set<T>) {
            T set;
            for (const auto &item : value) set.insert(fromJsonValue<typename T::value_type>(item));
            return set;
        } else if constexpr (detail::Sequence<T>) {
            T list;
            list.reserve(value.size());
            for (const auto &item : value) list.push_back(fromJsonValue<typename T::value_type>(item));
            return list;
        } else return value.get<T>();
    }

    template <class T, bool O, bool N>
    static void get(const nlohmann::json &object, std::string_view key, Field<T, O, N> &field)
    {
        const auto it = object.find(key);
        if (it == object.end()) { if constexpr (O) field.undefine(); return; }
        if (it->is_null()) { if constexpr (N) field = nullptr; return; }
        field = fromJsonValue<T>(*it);
    }
};

}
