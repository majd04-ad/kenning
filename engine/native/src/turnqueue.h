/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// One device (the camera) for many requests: served in the order they
// came, and at most one waiting or running request per user, so nobody can fill the queue.
// Root is trusted and not limited: the login manager and sudo ask as root.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sys/types.h>
#include <utility>

namespace FaceUnlock
{
class TurnQueue
{
public:
    // Held while the device is in use; the next request goes when it is destroyed.
    class Turn
    {
    public:
        Turn(TurnQueue &queue, std::uint64_t ticket, uid_t uid)
            : m_queue(&queue)
            , m_ticket(ticket)
            , m_uid(uid)
        {
        }
        Turn(Turn &&other) noexcept
            : m_queue(std::exchange(other.m_queue, nullptr))
            , m_ticket(other.m_ticket)
            , m_uid(other.m_uid)
        {
        }
        Turn(const Turn &) = delete;
        Turn &operator=(const Turn &) = delete;
        Turn &operator=(Turn &&) = delete;
        ~Turn()
        {
            if (m_queue) {
                m_queue->leave(m_ticket, m_uid);
            }
        }

    private:
        TurnQueue *m_queue;
        std::uint64_t m_ticket;
        uid_t m_uid;
    };

    // Waits for this request's turn. Empty when the user already has a request waiting or
    // running, or when cancelled() turned true while waiting.
    std::optional<Turn> take(uid_t uid, const std::function<bool()> &cancelled = {})
    {
        std::unique_lock lock(m_mutex);
        if (uid != 0 && m_users[uid] > 0) {
            return std::nullopt;
        }
        const std::uint64_t ticket = m_nextTicket++;
        m_waiting.push_back(ticket);
        ++m_users[uid];
        while (m_waiting.front() != ticket) {
            m_changed.wait_for(lock, std::chrono::milliseconds(200));
            if (m_waiting.front() != ticket && cancelled && cancelled()) {
                std::erase(m_waiting, ticket);
                release(uid);
                m_changed.notify_all();
                return std::nullopt;
            }
        }
        return Turn(*this, ticket, uid);
    }

    // Requests waiting or running, for tests and for deciding whether the service may exit.
    std::size_t size()
    {
        const std::lock_guard lock(m_mutex);
        return m_waiting.size();
    }

private:
    void leave(std::uint64_t ticket, uid_t uid)
    {
        const std::lock_guard lock(m_mutex);
        std::erase(m_waiting, ticket);
        release(uid);
        m_changed.notify_all();
    }

    void release(uid_t uid)
    {
        if (--m_users[uid] <= 0) {
            m_users.erase(uid);
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::uint64_t> m_waiting; // the front one holds the device
    std::map<uid_t, int> m_users;
    std::uint64_t m_nextTicket = 0;
};
}
