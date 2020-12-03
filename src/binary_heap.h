#ifndef MULTIQUEUE_BINARY_HEAP_H
#define MULTIQUEUE_BINARY_HEAP_H

#include <atomic>
#include <unordered_map>
#include <vector>
#include <limits>

#include "clh_mutex.h"

extern "C" {
    #include "clh.h"
}

class Spinlock {
private:
    std::atomic_flag spinlock = ATOMIC_FLAG_INIT;
public:
    void lock() {
        while (spinlock.test_and_set(std::memory_order_acquire));
    }
    void unlock() {
        spinlock.clear(std::memory_order_release);
    }
};

class CLHLock {
private:
    clh_mutex_t clh_lock;
public:
    CLHLock() {
        clh_mutex_init(&clh_lock);
    }
    void lock() {
        clh_mutex_lock(&clh_lock);
    }
    void unlock() {
        clh_mutex_unlock(&clh_lock);
    }
    ~CLHLock() {
        clh_mutex_destroy(&clh_lock);
    }
};

std::atomic<uint32_t> thread_counter{0};
thread_local uint32_t thread_id = thread_counter++;

class Params {
public:
    clh_local_params p;
    Params() {
        init_clh_local(thread_id, &p);
    }
    ~Params() {
        end_clh_local(p);
    }
};

class CLHLockLIBSLOCK {
private:
    clh_global_params the_lock;
    static thread_local std::unordered_map<CLHLockLIBSLOCK *, Params> m;
public:
    CLHLockLIBSLOCK() {
        init_clh_global(&the_lock);
        m.reserve(256);
    }
    void lock() {
        clh_local_params & p = m[this].p;
        p.my_pred = (clh_qnode*) clh_acquire(the_lock.the_lock, p.my_qnode);
    }
    void unlock() {
        clh_local_params & p = m[this].p;
        p.my_qnode = clh_release(p.my_qnode, p.my_pred);
    }
    ~CLHLockLIBSLOCK() {
//        end_clh_global(the_lock);
    }
};
thread_local std::unordered_map<CLHLockLIBSLOCK *, Params> CLHLockLIBSLOCK::m;

using Vertex = std::size_t;
using DistType = int;

class QueueElement {
private:
//    volatile char padding[128]{};
    std::atomic<DistType> dist;
    std::atomic<int> q_id;
    CLHLockLIBSLOCK empty_q_id_spinlock;  // lock when changing q_id from empty to something
public:
    size_t index;
    Vertex vertex;
    explicit QueueElement(Vertex vertex = 0, DistType dist = std::numeric_limits<DistType>::max()) : dist(dist), q_id(-1), vertex(vertex) {}
    QueueElement(const QueueElement & o) : dist(o.dist.load()), q_id(o.q_id.load()), vertex(o.vertex) {}
    void empty_q_id_lock() {
        empty_q_id_spinlock.lock();
    }
    void empty_q_id_unlock() {
        empty_q_id_spinlock.unlock();
    }
    [[noreturn]] QueueElement & operator=(const QueueElement & o) {
        (void)o;
        throw std::string("QueueElement.= shouldn't be used. Probably, BinHeap max size is exceeded.");
    }
    DistType get_dist() const {
        return dist.load();
    }
    void set_dist_relaxed(DistType new_dist) {
        dist.store(new_dist, std::memory_order_relaxed);
    }
    DistType get_dist_relaxed() const {
        return dist.load(std::memory_order_relaxed);
    }
    int get_q_id_relaxed() const {
        return q_id.load(std::memory_order_relaxed);
    }
    void set_q_id_relaxed(int new_q_id) {
        q_id.store(new_q_id, std::memory_order_relaxed);
    }
    bool operator==(const QueueElement & o) const {
        return o.vertex == vertex && o.get_dist() == get_dist();
    }
    bool operator!=(const QueueElement & o) const {
        return !operator==(o);
    }
    bool operator<(const QueueElement & o) const {
        return get_dist() > o.get_dist();
    }
    bool operator>(const QueueElement & o) const {
        return get_dist() < o.get_dist();
    }
    bool operator<=(const QueueElement & o) const {
        return get_dist() >= o.get_dist();
    }
    bool operator>=(const QueueElement & o) const {
        return get_dist() <= o.get_dist();
    }
};

static const DistType EMPTY_ELEMENT_DIST = -1;
static const QueueElement EMPTY_ELEMENT(0, EMPTY_ELEMENT_DIST);

class BinaryHeap {
private:
    size_t size = 0;
    std::vector<QueueElement *> elements;
    CLHLockLIBSLOCK spinlock;
    std::atomic<QueueElement *> top_element{const_cast<QueueElement *>(&EMPTY_ELEMENT)};

    void swap(size_t i, size_t j) {
        std::swap(elements[i], elements[j]);
        elements[i]->index = i;
        elements[j]->index = j;
    }
    void sift_up(size_t i) {
        if (size <= 1 || i == 0) {
            top_element.store(elements[0], std::memory_order_relaxed);
            return;
        };
        size_t p = get_parent(i); // everyone except for i == 0 has a parent
        while (*elements[i] > *elements[p]) {
            swap(i, p);
            i = p;
            if (i == 0) break;
            p = get_parent(i);
        }
        top_element.store(elements[0], std::memory_order_relaxed);
    }
    void sift_down(size_t i) {
        if (size == 0) {
            top_element.store(const_cast<QueueElement *>(&EMPTY_ELEMENT), std::memory_order_relaxed);
            return;
        }
        while (get_left_child(i) < size) {
            size_t l = get_left_child(i);
            size_t r = get_right_child(i);
            size_t j = r < size && *elements[r] > *elements[l] ? r : l;
            if (*elements[i] > *elements[j]) break;
            swap(i, j);
            i = j;
        }
        top_element.store(elements[0], std::memory_order_relaxed);
    }
    void set(size_t i, QueueElement * element) {
        elements[i] = element;
        elements[i]->index = i;
    }
    static inline size_t get_parent(size_t i) { return (i - 1) / 2; }
    static inline size_t get_left_child(size_t i) { return i * 2 + 1; }
    static inline size_t get_right_child(size_t i) { return i * 2 + 2; }
public:
    explicit BinaryHeap(size_t reserve_size = 256) {
        elements.reserve(reserve_size);
        elements.resize(1);
    }
    BinaryHeap(const BinaryHeap & o) : elements(std::vector<QueueElement *>(o.elements.capacity())) {}
    BinaryHeap& operator=(const BinaryHeap & o) {
        std::size_t reserve_size = o.elements.capacity();
        elements.reserve(reserve_size);
        return *this;
    }
    bool empty() const {
        return size == 0;
    }
    QueueElement * top() const {
        return empty() ? const_cast<QueueElement *>(&EMPTY_ELEMENT) : elements.front();
    }
    QueueElement * top_relaxed() const {
        return top_element.load(std::memory_order_relaxed);
    }
    void pop() {
        --size;
        elements[0]->index = -1;
        set(0, elements[size]);
        sift_down(0);
    }
    void push(QueueElement * element) {
        size++;
        if (size == elements.size()) {
            elements.resize(elements.size() * 4);
        }
        set(size - 1, element);
        sift_up(size - 1);
    }
    void decrease_key(QueueElement * element, int new_dist) {
        if (new_dist < element->get_dist()) { // redundant if?
            element->set_dist_relaxed(new_dist);
            size_t i = element->index;
            sift_up(i);
        }
    }
    void lock() {
        spinlock.lock();
    }
    void unlock() {
        spinlock.unlock();
    }
};

#endif //MULTIQUEUE_BINARY_HEAP_H
