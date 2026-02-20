// main.cpp
#include <iostream>
#include <vector>
#include <string>
#include <queue>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>

using namespace std;

// ---------- Helpers ----------
static inline string trim(const string& s) {
    size_t start = 0;
    while (start < s.size() && isspace((unsigned char)s[start])) start++;
    size_t end = s.size();
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    return s.substr(start, end - start);
}

static inline bool starts_with(const string& s, const string& prefix) {
    return s.size() >= prefix.size() && equal(prefix.begin(), prefix.end(), s.begin());
}

// ---------- Bounded Buffer ----------
class BoundedBuffer {
private:
    queue<string> buffer;
    int capacity;
    pthread_mutex_t mutex;
    sem_t full_slots;   // count of full slots
    sem_t empty_slots;  // count of empty slots

public:
    explicit BoundedBuffer(int size) : capacity(size) {
        pthread_mutex_init(&mutex, nullptr);
        sem_init(&full_slots, 0, 0);
        sem_init(&empty_slots, 0, size);
    }

    ~BoundedBuffer() {
        sem_destroy(&full_slots);
        sem_destroy(&empty_slots);
        pthread_mutex_destroy(&mutex);
    }

    void insert(const string& s) {
        sem_wait(&empty_slots);
        pthread_mutex_lock(&mutex);
        buffer.push(s);
        pthread_mutex_unlock(&mutex);
        sem_post(&full_slots);
    }

    string remove_blocking() {
        sem_wait(&full_slots);
        pthread_mutex_lock(&mutex);
        string s = buffer.front();
        buffer.pop();
        pthread_mutex_unlock(&mutex);
        sem_post(&empty_slots);
        return s;
    }

    // Non-blocking remove: required so Dispatcher won't block on empty queues
    bool tryRemove(string& out) {
        if (sem_trywait(&full_slots) != 0) {
            return false; // no item available (would block)
        }
        pthread_mutex_lock(&mutex);
        out = buffer.front();
        buffer.pop();
        pthread_mutex_unlock(&mutex);
        sem_post(&empty_slots);
        return true;
    }
};

// ---------- Unbounded Buffer ----------
class UnboundedBuffer {
private:
    queue<string> buffer;
    pthread_mutex_t mutex;
    sem_t full_slots;

public:
    UnboundedBuffer() {
        pthread_mutex_init(&mutex, nullptr);
        sem_init(&full_slots, 0, 0);
    }

    ~UnboundedBuffer() {
        sem_destroy(&full_slots);
        pthread_mutex_destroy(&mutex);
    }

    void insert(const string& s) {
        pthread_mutex_lock(&mutex);
        buffer.push(s);
        pthread_mutex_unlock(&mutex);
        sem_post(&full_slots);
    }

    string remove_blocking() {
        sem_wait(&full_slots);
        pthread_mutex_lock(&mutex);
        string s = buffer.front();
        buffer.pop();
        pthread_mutex_unlock(&mutex);
        return s;
    }
};

// ---------- Thread Data ----------
struct ProducerData {
    int id;                  // 1-based
    int items_to_produce;
    BoundedBuffer* q;
    unsigned int seed;
};

struct DispatcherData {
    vector<BoundedBuffer*>* prod_queues;
    UnboundedBuffer *s_q, *n_q, *w_q;
};

struct CoEditorData {
    UnboundedBuffer* in_q;
    BoundedBuffer* out_q;
};

struct ScreenData {
    BoundedBuffer* q;
};

// ---------- Producer ----------
void* producer_func(void* arg) {
    ProducerData* d = (ProducerData*)arg;
    const string types[3] = {"SPORTS", "NEWS", "WEATHER"};
    int counts[3] = {0, 0, 0};

    for (int i = 0; i < d->items_to_produce; i++) {
         
        string msg = "Producer " + to_string(d->id) + " " + types[t] + " " + to_string(counts[t]++);
        d->q->insert(msg);
    }
    d->q->insert("DONE");
    return nullptr;
}

// ---------- Dispatcher ----------
void* dispatcher_func(void* arg) {
    DispatcherData* d = (DispatcherData*)arg;
    int n = (int)d->prod_queues->size();
    vector<bool> finished(n, false);
    int active = n;

    while (active > 0) {
        bool didWork = false;

        for (int i = 0; i < n; i++) {
            if (finished[i]) continue;

            string msg;
            BoundedBuffer* q = (*d->prod_queues)[i];

            // Non-blocking remove: Dispatcher must NOT block on empty queues
            if (!q->tryRemove(msg)) {
                continue;
            }
            didWork = true;

            if (msg == "DONE") {
                finished[i] = true;
                active--;
                continue;
            }

            // sort by type -> dispatcher queues
            if (msg.find("SPORTS") != string::npos) d->s_q->insert(msg);
            else if (msg.find("NEWS") != string::npos) d->n_q->insert(msg);
            else if (msg.find("WEATHER") != string::npos) d->w_q->insert(msg);
        }

        // Avoid busy spinning when all queues are empty at the moment
        if (!didWork) {
            usleep(1000); // 1ms
        }
    }

    // DONE to each category queue
    d->s_q->insert("DONE");
    d->n_q->insert("DONE");
    d->w_q->insert("DONE");
    return nullptr;
}

// ---------- Co-Editor ----------
void* co_editor_func(void* arg) {
    CoEditorData* d = (CoEditorData*)arg;

    while (true) {
        string msg = d->in_q->remove_blocking();
        if (msg == "DONE") {
            d->out_q->insert("DONE"); // pass immediately
            break;
        }
        usleep(100000); // 0.1 sec
        d->out_q->insert(msg);
    }
    return nullptr;
}

// ---------- Screen Manager ----------
void* screen_manager_func(void* arg) {
    ScreenData* d = (ScreenData*)arg;
    int done_count = 0;

    while (done_count < 3) {
        string msg = d->q->remove_blocking();
        if (msg == "DONE") {
            done_count++;
        } else {
            cout << msg << endl;
        }
    }

    cout << "DONE" << endl;
    return nullptr;
}

// ---------- Config Parsing ----------
struct Config {
    vector<int> prod_counts;
    vector<int> prod_q_sizes;
    int co_editor_q_size = -1;
};

static bool parse_config(const string& path, Config& cfg) {
    ifstream in(path);
    if (!in) return false;

    string line;
    int currentProducer = 0;
    bool expectingCount = false;
    bool expectingQSize = false;

    while (getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (starts_with(line, "PRODUCER")) {
            // "PRODUCER i"
            currentProducer++;
            expectingCount = true;
            expectingQSize = false;
            continue;
        }

        if (starts_with(line, "Co-Editor queue size")) {
            // "Co-Editor queue size = X"
            size_t eq = line.find('=');
            if (eq == string::npos) return false;
            string rhs = trim(line.substr(eq + 1));
            cfg.co_editor_q_size = stoi(rhs);
            continue;
        }

        if (expectingCount) {
            // line is number of products
            cfg.prod_counts.push_back(stoi(line));
            expectingCount = false;
            expectingQSize = true;
            continue;
        }

        if (expectingQSize) {
            // "queue size = X"
            size_t eq = line.find('=');
            if (eq == string::npos) return false;
            string rhs = trim(line.substr(eq + 1));
            cfg.prod_q_sizes.push_back(stoi(rhs));
            expectingQSize = false;
            continue;
        }

        // If we got here: unexpected line (ignore or fail). We'll be strict:
        // return false;
    }

    if (cfg.co_editor_q_size <= 0) return false;
    if (cfg.prod_counts.empty()) return false;
    if (cfg.prod_counts.size() != cfg.prod_q_sizes.size()) return false;
    return true;
}

// ---------- main ----------
int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <config.txt>\n";
        return 1;
    }

    Config cfg;
    if (!parse_config(argv[1], cfg)) {
        cerr << "ERROR: failed to parse config file\n";
        return 1;
    }

    int num_producers = (int)cfg.prod_counts.size();

    // Create producer queues (bounded)
    vector<BoundedBuffer*> prod_queues;
    prod_queues.reserve(num_producers);
    for (int i = 0; i < num_producers; i++) {
        prod_queues.push_back(new BoundedBuffer(cfg.prod_q_sizes[i]));
    }

    // Dispatcher queues (unbounded)
    UnboundedBuffer s_q, n_q, w_q;

    // Shared queue from Co-Editors to Screen (bounded)
    BoundedBuffer screen_q(cfg.co_editor_q_size);

    // Thread objects
    vector<pthread_t> producer_threads(num_producers);
    pthread_t dispatcher_thread;
    pthread_t coedit_threads[3];
    pthread_t screen_thread;

    // Producer data
    vector<ProducerData> producers(num_producers);
    unsigned int base_seed = (unsigned int)time(nullptr);

    for (int i = 0; i < num_producers; i++) {
        producers[i].id = i + 1;
        producers[i].items_to_produce = cfg.prod_counts[i];
        producers[i].q = prod_queues[i];
        producers[i].seed = base_seed ^ (unsigned int)(i * 2654435761u);
        pthread_create(&producer_threads[i], nullptr, producer_func, &producers[i]);
    }

    // Dispatcher data
    DispatcherData dd;
    dd.prod_queues = &prod_queues;
    dd.s_q = &s_q;
    dd.n_q = &n_q;
    dd.w_q = &w_q;
    pthread_create(&dispatcher_thread, nullptr, dispatcher_func, &dd);

    // Co-editor data (3 types)
    CoEditorData ce_s{&s_q, &screen_q};
    CoEditorData ce_n{&n_q, &screen_q};
    CoEditorData ce_w{&w_q, &screen_q};
    pthread_create(&coedit_threads[0], nullptr, co_editor_func, &ce_s);
    pthread_create(&coedit_threads[1], nullptr, co_editor_func, &ce_n);
    pthread_create(&coedit_threads[2], nullptr, co_editor_func, &ce_w);

    // Screen manager
    ScreenData sd{&screen_q};
    pthread_create(&screen_thread, nullptr, screen_manager_func, &sd);

    // Join all
    for (int i = 0; i < num_producers; i++) {
        pthread_join(producer_threads[i], nullptr);
    }
    pthread_join(dispatcher_thread, nullptr);
    pthread_join(coedit_threads[0], nullptr);
    pthread_join(coedit_threads[1], nullptr);
    pthread_join(coedit_threads[2], nullptr);
    pthread_join(screen_thread, nullptr);

    // Cleanup
    for (auto* q : prod_queues) delete q;

    return 0;
}
