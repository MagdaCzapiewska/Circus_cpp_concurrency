#ifndef SYSTEM_HPP
#define SYSTEM_HPP

#include <exception>
#include <vector>
#include <unordered_map>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <semaphore>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <utility>
#include <thread>

#include "machine.hpp"

class FulfillmentFailure : public std::exception
{
};

class OrderNotReadyException : public std::exception
{
};

class BadOrderException : public std::exception
{
};

class BadPagerException : public std::exception
{
};

class OrderExpiredException : public std::exception
{
};

class RestaurantClosedException : public std::exception
{
};

class NullProductException : public std::exception
{
};

struct WorkerReport
{
    std::vector<std::vector<std::string>> collectedOrders;
    std::vector<std::vector<std::string>> abandonedOrders;
    std::vector<std::vector<std::string>> failedOrders;
    std::vector<std::string> failedProducts;
};

struct OrderFeatures
{
    unsigned int order_id;
    std::vector<std::string> ordered_products;
};


class System;

class CoasterPager
{
    friend class System;

protected:
    unsigned int id;
    System *sys; // Musi być przez wskaźnik, bo inaczej nie wiadomo, ile pamięci zaalokować
    explicit CoasterPager(unsigned int id, System *sys) : id(id), sys(sys) {};

public:
    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const;

    [[nodiscard]] bool isReady() const;
};


class System
{
    friend void workerFunction(unsigned int my_id, System &sys);
    friend void machineControllerFunction(unsigned int machine_id, std::string product_name, std::shared_ptr<Machine> my_machine, System &sys);
    friend class CoasterPager;
public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;

protected:
    unsigned int numOfWorkers;

    std::vector<struct WorkerReport> reports; // Report of every worker
    std::vector<std::unique_ptr<std::mutex>> mut_for_one_report; // Mutex to write into reports (various machines update failedProducts)

    std::unordered_map<unsigned int, unsigned int> responsibility_map; // Key - order id, value - (worker id, finished)
    std::mutex mut_responsibility_map; // Mutex to avoid data races in reading / writing to responsibility_map

    std::queue<struct OrderFeatures> orders_queue; // Queue for orders
    mutable std::mutex mut_shut_and_orders_queue; // Mutex to avoid data races in changing orders_queue and is_shut

    std::condition_variable free_workers; // Condition variable for workers ready to work

    unsigned int how_many_orders = 0; // Used for giving numbers to coming orders.
    //std::mutex mut_how_many_orders; // Used for changing how_many_orders and putting order in a queue

    std::vector<int> order_products_left; // k means there are k products left to get
    std::vector<int> order_status; // 0 - ready. 1, 2, 3, 4 - exceptions
    std::vector<std::unique_ptr<std::condition_variable>> pager_waits; // Condition variables for pagers to wait for order completion
    mutable std::mutex mut_order_status; // Mutex to avoid data races in reading / writing to order_status

    machines_t machines; // Map for product name + shared pointer to the machine
    mutable std::mutex mut_machines; // Mutex to avoid data races in reading / writing to machines
    machines_t machines_forever;

    unsigned int client_timeout;

    std::vector<unsigned int> pending_orders; // Vector for ids of orders that are not ready yet and not cancelled
    mutable std::mutex mut_pending_orders;

    std::vector<std::thread> workers;

    bool is_shut = false;

    std::vector<std::vector<std::pair<std::string, std::unique_ptr<Product>>>> ownership; // Vector for products that workers own at the time
    std::vector<std::unique_ptr<std::mutex>> mut_ownership;

    std::vector<std::thread> controllers;

    std::vector<std::queue<unsigned int>> machine_queue; // Vectors for queues to machines (order id written into it)
    std::vector<std::unique_ptr<std::mutex>> mut_machine_queue; // Mutex to avoid data races in reading/writing to machine_queue

    std::unordered_map<std::string, unsigned int> product_id; // Product and it's id (and also id of the machine)
    std::vector<std::unique_ptr<std::mutex>> mut_return;

    std::vector<std::unique_ptr<std::condition_variable>> worker_waits_for_order_completion; // Condition variable for workers to wait for order completion
    std::vector<std::unique_ptr<std::condition_variable>> worker_waits_for_order_collection; // Condition variable for workers to wait for order collection
    std::vector<std::unique_ptr<std::counting_semaphore<>>> controller_waits; // Semaphore for controller to wait for having something in the queue

public:

    System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout);

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const;

    std::vector<unsigned int> getPendingOrders() const;

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products);

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager);

    unsigned int getClientTimeout() const;
};

#endif // SYSTEM_HPP