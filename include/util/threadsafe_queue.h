#pragma once

#include <queue>
#include <mutex>

namespace metafs {

template<typename T>
class threadsafe_queue {
    std::queue<T> q;
    mutable std::mutex m;

public:
    threadsafe_queue() {}

    bool empty() const {
        std::scoped_lock lock(m);
        return q.empty();
    }

    void push(T element) {
        std::scoped_lock lock(m);
        q.push(std::move(element));
    }

    bool pop(T& element) {
        std::scoped_lock lock(m);
        if (!q.empty()) {
            element = q.front();
            q.pop();
            return true;
        }
        return false;
    }
};

}