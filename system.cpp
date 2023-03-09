#include "system.hpp"
#include <limits>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

void workerFunction(unsigned int my_id, System &sys) {
    while (true) {
        std::unique_lock<std::mutex> lock_is_shut_and_queue(sys.mut_shut_and_orders_queue);
        sys.free_workers.wait(lock_is_shut_and_queue,
                              [&sys] {return (!sys.orders_queue.empty()) || (sys.orders_queue.empty() && sys.is_shut);});
        if (sys.orders_queue.empty() && sys.is_shut) {
            break;
        }
        unsigned int order_id;
        struct OrderFeatures my_order;

        my_order = sys.orders_queue.front();
        sys.orders_queue.pop();
        order_id = my_order.order_id;

        std::unique_lock<std::mutex> lock_resp(sys.mut_responsibility_map);
        sys.responsibility_map[order_id] = my_id;
        lock_resp.unlock();

        for (std::string s : my_order.ordered_products) {
            unsigned int prod_id = sys.product_id[s];
            std::unique_lock<std::mutex> lock_mq(*(sys.mut_machine_queue[prod_id]));
            sys.machine_queue[prod_id].push(order_id);
            lock_mq.unlock();
            sys.controller_waits[prod_id]->release();
        }

        lock_is_shut_and_queue.unlock();

        std::unique_lock<std::mutex> lock_status(sys.mut_order_status);
        sys.worker_waits_for_order_completion[my_id]->wait(lock_status,
                                                           [&sys, &order_id] {return sys.order_status[order_id] >= 0 && sys.order_products_left[order_id] == 0;});

        if (sys.order_status[order_id] == 0) {

            sys.worker_waits_for_order_collection[my_id]->wait_for(
                    lock_status,std::chrono::milliseconds(sys.client_timeout), [&sys, &order_id, &my_id] { return sys.order_status[order_id] == 4; });

            if (sys.order_status[order_id] == 0) {
                sys.order_status[order_id] = 3;

                std::unique_lock<std::mutex> lock_pending(sys.mut_pending_orders);
                if (!sys.pending_orders.empty()) {
                    bool found = false;
                    auto it = sys.pending_orders.begin();
                    for (it = sys.pending_orders.begin(); it != sys.pending_orders.end(); ++it) {
                        if ((*it) == order_id) {
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        sys.pending_orders.erase(it);
                    }
                }
                lock_pending.unlock();

                std::vector<std::string> v;
                for (const std::string& s : my_order.ordered_products) {
                    v.push_back(s);
                }
                std::unique_lock<std::mutex> lock_report(*(sys.mut_for_one_report[my_id]));
                sys.reports[my_id].abandonedOrders.push_back(v);
                lock_report.unlock();
                std::unique_lock<std::mutex> lock_own(*(sys.mut_ownership[my_id]));
                for (auto it = sys.ownership[my_id].begin(); it != sys.ownership[my_id].end(); ++it) {
                    std::unique_lock<std::mutex> lock_return(*(sys.mut_return[sys.product_id[it->first]]));
                    sys.machines_forever[it->first]->returnProduct(std::move(it->second));
                    lock_return.unlock();
                }
                sys.ownership[my_id].clear();
                lock_own.unlock();
            }
            else if (sys.order_status[order_id] == 4) {
                std::vector<std::string> v;
                for (const std::string& s : my_order.ordered_products) {
                    v.push_back(s);
                }
                std::unique_lock<std::mutex> lock_report(*(sys.mut_for_one_report[my_id]));
                sys.reports[my_id].collectedOrders.push_back(v);
                lock_report.unlock();
                std::unique_lock<std::mutex> lock_own(*(sys.mut_ownership[my_id]));
                sys.ownership[my_id].clear();
                lock_own.unlock();
            }
        }
        else if (sys.order_status[order_id] == 1){ // sys.order_status[order_id] == 1
            std::vector<std::string> v;
            for (const std::string& s : my_order.ordered_products) {
                v.push_back(s);
            }
            std::unique_lock<std::mutex> lock_report(*(sys.mut_for_one_report[my_id]));
            sys.reports[my_id].failedOrders.push_back(v);
            lock_report.unlock();
            std::unique_lock<std::mutex> lock_own(*(sys.mut_ownership[my_id]));
            for (auto it = sys.ownership[my_id].begin(); it != sys.ownership[my_id].end(); ++it) {
                std::unique_lock<std::mutex> lock_return(*(sys.mut_return[sys.product_id[it->first]]));
                sys.machines_forever[it->first]->returnProduct(std::move(it->second));
                lock_return.unlock();
            }
            sys.ownership[my_id].clear();
            lock_own.unlock();
        }
        else if (sys.order_status[order_id] == 4) {
            std::vector<std::string> v;
            for (const std::string& s : my_order.ordered_products) {
                v.push_back(s);
            }
            std::unique_lock<std::mutex> lock_report(*(sys.mut_for_one_report[my_id]));
            sys.reports[my_id].collectedOrders.push_back(v);
            lock_report.unlock();
            std::unique_lock<std::mutex> lock_own(*(sys.mut_ownership[my_id]));
            sys.ownership[my_id].clear();
            lock_own.unlock();
        }
    }
}

void machineControllerFunction(unsigned int machine_id, std::string product_name, std::shared_ptr<Machine> my_machine, System &sys) {
    bool machine_failure = false;
    while (true) {
        sys.controller_waits[machine_id]->acquire();
        std::unique_lock<std::mutex> lock_shut(sys.mut_shut_and_orders_queue);
        std::unique_lock<std::mutex> lock_mq(*(sys.mut_machine_queue[machine_id]));

        if (sys.is_shut && sys.machine_queue[machine_id].empty() && sys.orders_queue.empty()) {
            break;
        }

        unsigned int order_id = sys.machine_queue[machine_id].front();
        sys.machine_queue[machine_id].pop();

        lock_mq.unlock();
        lock_shut.unlock();

        unsigned int worker_id;
        std::unique_lock<std::mutex> lock_resp(sys.mut_responsibility_map);
        worker_id = sys.responsibility_map[order_id];
        lock_resp.unlock();

        bool ending = false;

        if (machine_failure) {

            std::unique_lock<std::mutex> lock_pending(sys.mut_pending_orders);
            if (!sys.pending_orders.empty()) {
                bool found = false;
                auto it = sys.pending_orders.begin();
                for (it = sys.pending_orders.begin(); it != sys.pending_orders.end(); ++it) {
                    if ((*it) == order_id) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    sys.pending_orders.erase(it);
                }
            }
            lock_pending.unlock();

            std::unique_lock<std::mutex> lock_report(*(sys.mut_for_one_report[worker_id]));
            sys.reports[worker_id].failedProducts.push_back(product_name);
            lock_report.unlock();

            std::unique_lock<std::mutex> lock_status(sys.mut_order_status);
            sys.order_products_left[order_id]--;
            sys.order_status[order_id] = 1;
            if (sys.order_products_left[order_id] == 0) {
                ending = true;
            }
            lock_status.unlock();

            if (ending) {
                sys.worker_waits_for_order_completion[worker_id]->notify_one();
                sys.pager_waits[order_id]->notify_one();
            }
        }
        else {
            try {
                std::unique_ptr<Product> product = my_machine->getProduct();

                if (product == nullptr) {
                    throw NullProductException();
                }
                std::unique_lock<std::mutex> lock_own(*(sys.mut_ownership[worker_id]));
                sys.ownership[worker_id].emplace_back(product_name, std::move(product));
                lock_own.unlock();

                std::unique_lock<std::mutex> lock_status(sys.mut_order_status);
                sys.order_products_left[order_id]--;
                if (sys.order_products_left[order_id] == 0) {
                    if (sys.order_status[order_id] != 1) {
                        sys.order_status[order_id] = 0;
                    }
                    ending = true;
                }
                lock_status.unlock();

                if (ending) {
                    sys.worker_waits_for_order_completion[worker_id]->notify_one();
                    sys.pager_waits[order_id]->notify_one();
                }
            }
            catch (const MachineFailure &e) {

                std::unique_lock<std::mutex> lock_pending(sys.mut_pending_orders);
                if (!sys.pending_orders.empty()) {
                    bool found = false;
                    auto it = sys.pending_orders.begin();
                    for (it = sys.pending_orders.begin(); it != sys.pending_orders.end(); ++it) {
                        if ((*it) == order_id) {
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        sys.pending_orders.erase(it);
                    }
                }
                lock_pending.unlock();

                machine_failure = true;
                std::unique_lock<std::mutex> lock_machines(sys.mut_machines);
                if (sys.machines.contains(product_name)) {
                    sys.machines.erase(product_name);
                }
                lock_machines.unlock();

                std::unique_lock<std::mutex> lock_report(*(sys.mut_for_one_report[worker_id]));
                sys.reports[worker_id].failedProducts.push_back(product_name);
                lock_report.unlock();

                std::unique_lock<std::mutex> lock_status(sys.mut_order_status);
                sys.order_products_left[order_id]--;
                sys.order_status[order_id] = 1;
                if (sys.order_products_left[order_id] == 0) {
                    ending = true;
                }
                lock_status.unlock();

                if (ending) {
                    sys.worker_waits_for_order_completion[worker_id]->notify_one();
                    sys.pager_waits[order_id]->notify_one();
                }
            }
        }
    }
}

void CoasterPager::wait() const {

    std::unique_lock<std::mutex> lock_status(sys->mut_order_status);
    sys->pager_waits[id]->wait(lock_status, [this] { return sys->order_status[id] >= 0; });

    if (sys->order_status[id] == 1) {
        throw FulfillmentFailure();
    }
}

void CoasterPager::wait(unsigned int timeout) const {

    std::unique_lock<std::mutex> lock_status(sys->mut_order_status);
    sys->pager_waits[id]->wait_for(
            lock_status, std::chrono::milliseconds(timeout), [this] { return sys->order_status[id] >= 0; });

    if (sys->order_status[id] == 1) {
        throw FulfillmentFailure();
    }
}

unsigned int CoasterPager::getId() const {
    return id;
}

bool CoasterPager::isReady() const {

    bool ready = false;
    std::unique_lock<std::mutex> lock_status(sys->mut_order_status);
    if (sys->order_status[id] == 0) {
        ready = true;
    }
    return ready;
}

System::System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout) {
    this->client_timeout = clientTimeout;
    this->numOfWorkers = numberOfWorkers;

    unsigned int machine_id = 0;
    for (auto it = machines.begin(); it != machines.end(); ++it) {
        this->machines[it->first] = it->second;
        this->machines_forever[it->first] = it->second;

        std::queue<unsigned int> machine_q;
        machine_queue.push_back(std::move(machine_q));
        std::unique_ptr<std::mutex> mut_mq = std::make_unique<std::mutex>();
        mut_machine_queue.push_back(std::move(mut_mq));

        product_id[it->first] = machine_id;
        std::unique_ptr<std::mutex> up_mut = std::make_unique<std::mutex>();
        mut_return.push_back(std::move(up_mut));

        std::unique_ptr<std::counting_semaphore<>> up_cs = std::make_unique<std::counting_semaphore<INT_MAX>>(0);
        controller_waits.push_back(std::move(up_cs));

        machine_id++;
    }

    machine_id = 0;
    for (auto it = machines.begin(); it != machines.end(); ++it) {
        (*it).second->start();
        std::thread controller{machineControllerFunction, machine_id, (*it).first, (*it).second, std::ref(*this)};
        controllers.push_back(std::move(controller));
        machine_id++;
    }

    for (unsigned int i = 0; i < numberOfWorkers; ++i) {
        struct WorkerReport w;
        reports.push_back(std::move(w));
        std::unique_ptr<std::mutex> mut_report = std::make_unique<std::mutex>();
        mut_for_one_report.push_back(std::move(mut_report));

        std::vector<std::pair<std::string, std::unique_ptr<Product>>> own;
        ownership.push_back(std::move(own));
        std::unique_ptr<std::mutex> mut_own = std::make_unique<std::mutex>();
        mut_ownership.push_back(std::move(mut_own));

        std::unique_ptr<std::condition_variable> up_cv1 = std::make_unique<std::condition_variable>();
        worker_waits_for_order_completion.push_back(std::move(up_cv1));

        std::unique_ptr<std::condition_variable> up_cv2 = std::make_unique<std::condition_variable>();
        worker_waits_for_order_collection.push_back(std::move(up_cv2));

        std::thread worker{workerFunction, i, std::ref(*this)};
        workers.push_back(std::move(worker));
    }
}

std::vector<WorkerReport> System::shutdown() {


    std::unique_lock<std::mutex> lock_is_shut(mut_shut_and_orders_queue);
    if (!is_shut) {
        is_shut = true;
        lock_is_shut.unlock();
        free_workers.notify_all();
        for (auto &t : workers) {
            t.join();
        }

        for (auto it = controller_waits.begin(); it != controller_waits.end(); ++it) {
            (*it)->release();
        }

        for (auto &t : controllers) {
            t.join();
        }

        std::unique_lock<std::mutex> lock_machines(mut_machines);
        for (auto it = machines.begin(); it != machines.end(); ++it) {
            (*it).second->stop();
        }
        lock_machines.unlock();

        for (unsigned int i = 0; i < numOfWorkers; ++i) {
            std::unique_lock<std::mutex> lock_rep(*(mut_for_one_report[i]));

            if (!reports[i].failedProducts.empty()) {
                std::sort(reports[i].failedProducts.begin(), reports[i].failedProducts.end());
                std::string last_name = *(reports[i].failedProducts.begin());
                for (auto it = reports[i].failedProducts.begin() + 1; it != reports[i].failedProducts.end();) {
                    if (*it == last_name) {
                        it = reports[i].failedProducts.erase(it);
                    }
                    else {
                        last_name = *it;
                        ++it;
                    }
                }
            }
            lock_rep.unlock();
        }
    }
    else {
        lock_is_shut.unlock();
    }
    return reports;
}

std::vector<std::string> System::getMenu() const {
    std::vector<std::string> result;

    std::unique_lock<std::mutex> lock_is_shut(mut_shut_and_orders_queue);
    std::unique_lock<std::mutex> lock_machines(mut_machines);

    if (!is_shut) {
        for (auto it = machines.begin(); it != machines.end(); ++it) {
            result.push_back((*it).first);
        }
    }

    return result;
}

std::vector<unsigned int> System::getPendingOrders() const {
    std::vector<unsigned int> result;

    std::unique_lock<std::mutex> lock_pending(mut_pending_orders);
    for (auto it = pending_orders.begin(); it != pending_orders.end(); ++it) {
        result.push_back(*it);
    }
    return result;
}

unsigned int System::getClientTimeout() const {
    return client_timeout;
}


std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    std::unique_lock<std::mutex> lock_shut(mut_shut_and_orders_queue);
    if (is_shut) {
        throw RestaurantClosedException(); // Nie trzeba zwalniać lock_shut, bo sam się zwolni
    }
    else {
        std::unique_lock<std::mutex> lock_map(mut_machines);
        bool everything_in_menu = true;
        for (const std::string& s : products) {
            if (!machines.contains(s)) {
                everything_in_menu = false;
                break;
            }
        }
        lock_map.unlock();
        if (!everything_in_menu || products.empty()) {
            throw BadOrderException(); // Nie trzeba zwalniać lock_shut i lock_map, bo same się zwolnią
        }
        else {
            struct OrderFeatures new_order;
            new_order.order_id = how_many_orders;
            for (std::string s : products) {
                new_order.ordered_products.push_back(s);
            }

            orders_queue.push(std::move(new_order));
            std::unique_lock<std::mutex> lock_status(mut_order_status);
            order_products_left.push_back((int)(products.size()));
            order_status.push_back(-1);
            std::unique_ptr<std::condition_variable> up_cv = std::make_unique<std::condition_variable>();
            pager_waits.push_back(std::move(up_cv));
            lock_status.unlock();

            std::unique_lock<std::mutex> lock_pending(mut_pending_orders);
            pending_orders.push_back(how_many_orders);
            lock_pending.unlock();

            CoasterPager pager(how_many_orders, this);
            std::unique_ptr<CoasterPager> up_cp = std::make_unique<CoasterPager>(pager);

            how_many_orders++;
            lock_shut.unlock();

            free_workers.notify_one();

            return up_cp;
        }
    }
}

std::vector<std::unique_ptr<Product>> System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {

    if (CoasterPager == nullptr) {
        throw BadPagerException();
    }

    std::vector<std::unique_ptr<Product>> result;

    unsigned int order_id = CoasterPager->getId();
    std::unique_lock<std::mutex> lock_status(mut_order_status);

    if (order_status[order_id] == 1 && order_products_left[order_id] == 0) {
        throw FulfillmentFailure();
    }
    else if (order_status[order_id] == 1 && order_products_left[order_id] > 0) {
        throw OrderNotReadyException();
    }
    else if (order_status[order_id] == -1) {
        throw OrderNotReadyException();
    }
    else if (order_status[order_id] == 3) {
        throw OrderExpiredException();
    }
    else if (order_status[order_id] == 4) {
        throw BadPagerException();
    }
    else {
        std::unique_lock<std::mutex> lock_resp(mut_responsibility_map);
        unsigned int worker_id = responsibility_map[order_id];
        lock_resp.unlock();

        std::unique_lock<std::mutex> lock_own(*(mut_ownership[worker_id]));
        for (auto it = ownership[worker_id].begin(); it != ownership[worker_id].end(); ++it) {
            result.push_back(std::move((*it).second));
        }
        ownership[worker_id].clear();
        lock_own.unlock();

        std::unique_lock<std::mutex> lock_pending(mut_pending_orders);
        if (!pending_orders.empty()) {
            bool found = false;
            auto it = pending_orders.begin();
            for (it = pending_orders.begin(); it != pending_orders.end(); ++it) {
                if ((*it) == order_id) {
                    found = true;
                    break;
                }
            }
            if (found) {
                pending_orders.erase(it);
            }
        }
        lock_pending.unlock();

        order_status[order_id] = 4;


        lock_status.unlock();
        worker_waits_for_order_collection[worker_id]->notify_one();
    }

    return result;
}