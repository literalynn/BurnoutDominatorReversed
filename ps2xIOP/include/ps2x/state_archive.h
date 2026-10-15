#pragma once

// Binary archive for save states. One serialize(StateArchive&) function per
// component both writes and reads, so the two directions cannot drift apart.
// Tags between sections turn a layout mismatch into an error instead of
// silently corrupted state.

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace ps2x
{
    class StateArchive
    {
    public:
        static StateArchive writer() { return StateArchive(false, {}); }
        // version: format version of the data, for components whose layout changed.
        static StateArchive reader(std::vector<uint8_t> data, uint32_t version = 0)
        {
            StateArchive archive(true, std::move(data));
            archive.m_version = version;
            return archive;
        }

        bool loading() const { return m_loading; }
        uint32_t version() const { return m_version; }
        const std::vector<uint8_t> &data() const { return m_data; }
        bool atEnd() const { return m_pos == m_data.size(); }

        void raw(void *data, size_t size)
        {
            if (m_loading)
            {
                if (m_pos + size > m_data.size())
                    throw std::runtime_error("save state is truncated");
                std::memcpy(data, m_data.data() + m_pos, size);
                m_pos += size;
            }
            else
            {
                const auto *bytes = static_cast<const uint8_t *>(data);
                m_data.insert(m_data.end(), bytes, bytes + size);
            }
        }

        template <class T>
        void value(T &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "use a dedicated overload");
            raw(&v, sizeof(T));
        }

        // Writes a marker, or checks it when loading.
        void tag(const char (&name)[5])
        {
            uint32_t expected = 0;
            std::memcpy(&expected, name, 4);
            uint32_t found = expected;
            value(found);
            if (found != expected)
                throw std::runtime_error(std::string("save state section mismatch at ") + name);
        }

        void string(std::string &s)
        {
            uint64_t size = s.size();
            value(size);
            if (m_loading)
                s.resize(static_cast<size_t>(size));
            if (size)
                raw(s.data(), static_cast<size_t>(size));
        }

        template <class T>
        void vector(std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "use sequence() for non-trivial elements");
            uint64_t size = v.size();
            value(size);
            if (m_loading)
                v.resize(static_cast<size_t>(size));
            if (size)
                raw(v.data(), static_cast<size_t>(size) * sizeof(T));
        }

        // Any resizable container; each(archive, element) serializes one element.
        template <class C, class F>
        void sequence(C &container, F &&each)
        {
            uint64_t size = container.size();
            value(size);
            if (m_loading)
            {
                container.clear();
                container.resize(static_cast<size_t>(size));
            }
            for (auto &element : container)
                each(*this, element);
        }

        // std::map / std::unordered_map; each(archive, value) serializes one mapped value.
        template <class M, class F>
        void map(M &m, F &&each)
        {
            uint64_t size = m.size();
            value(size);
            if (m_loading)
            {
                m.clear();
                for (uint64_t i = 0; i < size; ++i)
                {
                    typename M::key_type key{};
                    keyValue(key);
                    each(*this, m[key]);
                }
            }
            else
            {
                for (auto &[key, mapped] : m)
                {
                    auto copy = key;
                    keyValue(copy);
                    each(*this, mapped);
                }
            }
        }

        template <class M>
        void podMap(M &m)
        {
            map(m, [](StateArchive &a, auto &v) { a.value(v); });
        }

        template <class T>
        void optional(std::optional<T> &o)
        {
            bool present = o.has_value();
            value(present);
            if (m_loading)
            {
                if (present)
                    o.emplace();
                else
                    o.reset();
            }
            if (present)
                value(*o);
        }

    private:
        StateArchive(bool loading, std::vector<uint8_t> data) : m_loading(loading), m_data(std::move(data)) {}

        template <class K>
        void keyValue(K &key)
        {
            if constexpr (std::is_same_v<K, std::string>)
                string(key);
            else
                value(key);
        }

        bool m_loading = false;
        uint32_t m_version = 0;
        std::vector<uint8_t> m_data;
        size_t m_pos = 0;
    };
}
