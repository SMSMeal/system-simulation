#include <iostream>
#include <vector>
#include <queue>
#include <random>
#include <algorithm>
#include <iomanip>
#include <string>
#include <chrono>
#include <cmath>

using namespace std;

// ========================= 常量 =========================
const int TOTAL_STUDENTS = 3850;
const int GROUP1_COUNT = 1880;
const int GROUP2_COUNT = 1970;
const double GROUP1_RELEASE = 10.0;   // 12:10
const double GROUP2_RELEASE = 25.0;   // 12:25
const double OUTPUT_INTERVAL = 1.0 / 6.0; // 10秒

// ========================= 食堂与窗口 =========================
enum Canteen { CANTEEN_SHUYUAN = 0, CANTEEN_YIYUAN1, CANTEEN_YIYUAN2, CANTEEN_YIYUAN_SPECIAL };
const int CANTEEN_COUNT = 4;

struct Window {
    string name;
    double t_sec, t_min;
    queue<int> waiting_queue;
    double service_end_time;
    int canteen_id;
    Window(string n, double s, int cid) : name(n), t_sec(s), t_min(s / 60.0), service_end_time(0.0), canteen_id(cid) {}
};

vector<Window> windows;
int canteen_window_count[CANTEEN_COUNT] = {0};
vector<int> canteen_window_indices[CANTEEN_COUNT];

void init_windows() {
    auto add = [&](string name, double t, int count, int cid) {
        for (int i = 0; i < count; ++i) {
            string full_name = name + (count > 1 ? "-" + to_string(i+1) : "");
            windows.emplace_back(full_name, t, cid);
            canteen_window_indices[cid].push_back((int)windows.size() - 1);
        }
        canteen_window_count[cid] += count;
    };
    add("澍园套餐", 6.0, 2, CANTEEN_SHUYUAN);
    add("澍园自选", 17.8, 3, CANTEEN_SHUYUAN);
    add("澍园湖南小碗菜", 18.7, 3, CANTEEN_SHUYUAN);
    add("澍园减脂餐", 21.8, 1, CANTEEN_SHUYUAN);
    add("澍园焗饭", 12.9, 1, CANTEEN_SHUYUAN);
    add("澍园西餐", 16.85, 1, CANTEEN_SHUYUAN);
    add("澍园叉烧", 11.9, 1, CANTEEN_SHUYUAN);
    add("澍园粉面", 12.7, 1, CANTEEN_SHUYUAN);
    add("澍园小笼包", 73.4, 1, CANTEEN_SHUYUAN);
    add("宜园一楼自选", 13.0, 3, CANTEEN_YIYUAN1);
    add("宜园二楼自选", 17.3, 3, CANTEEN_YIYUAN2);
    add("宜园二楼粉面", 17.5, 1, CANTEEN_YIYUAN2);
    add("宜园特色云吞/面", 16.8, 1, CANTEEN_YIYUAN_SPECIAL);
    add("宜园特色麻辣烫", 90.0, 1, CANTEEN_YIYUAN_SPECIAL);
    add("宜园特色西餐", 20.0, 1, CANTEEN_YIYUAN_SPECIAL);
    add("宜园特色粉面", 16.2, 1, CANTEEN_YIYUAN_SPECIAL);
    add("宜园特色蛋包饭/意面", 9.0, 1, CANTEEN_YIYUAN_SPECIAL);
}

// ========================= 学生 =========================
struct Student {
    int id;
    double ready_time;
    double walk_time;
    bool use_strategy;
    int chosen_window;
    double enqueue_time;
    double service_start;
    double wait_time;
    bool is_control;
    bool is_exp_use;
    bool is_exp_notuse;
};

vector<Student> students(TOTAL_STUDENTS);

// ========================= 随机数 =========================
mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());

int rand_delay() { static uniform_int_distribution<int> d(-3,3); return d(rng); }
int rand_walk()  { static uniform_int_distribution<int> d(1,5); return d(rng); }
int rand_0_9()   { static uniform_int_distribution<int> d(0,9); return d(rng); }
double rand_01() { static uniform_real_distribution<double> d(0.0,1.0); return d(rng); }

// 基于拥堵等级的等待时间（重新评估阈值）
double get_wait_time_by_congestion(int total_queue) {
    if (total_queue > 150) {
        static uniform_real_distribution<double> d(12.0, 22.0);
        return d(rng);
    } else if (total_queue > 80) {
        static uniform_real_distribution<double> d(5.0, 12.0);
        return d(rng);
    } else {
        static uniform_real_distribution<double> d(0.0, 6.0);
        return d(rng);
    }
}

// ========================= 全局状态 =========================
double current_time = 0.0;
int served_count = 0;
bool simulation_done = false;

// ========================= 事件系统 =========================
enum EventType { ARRIVAL, STRATEGY_DECISION, ENQUEUE, SERVICE_DONE, OUTPUT };

struct Event {
    double time;
    EventType type;
    int student_id;
    int window_idx;
    bool operator>(const Event& other) const {
        if (fabs(time - other.time) > 1e-9) return time > other.time;
        return type > other.type;
    }
};

priority_queue<Event, vector<Event>, greater<Event>> event_queue;

// ========================= 辅助函数 =========================

double get_wait_time(int win_idx, double now) {
    const Window& w = windows[win_idx];
    double remain = max(0.0, w.service_end_time - now);
    double queue_wait = (double)w.waiting_queue.size() * w.t_min;
    return remain + queue_wait;
}

// ---------- 非策略选择（按食堂加权 + 最短排队） ----------
int select_window_non_strategy() {
    double total = 0;
    for (int c = 0; c < CANTEEN_COUNT; ++c) total += canteen_window_count[c];
    double r = rand_01() * total;
    double accum = 0.0;
    int selected = 0;
    for (int c = 0; c < CANTEEN_COUNT; ++c) {
        accum += canteen_window_count[c];
        if (r <= accum) { selected = c; break; }
    }
    const auto& indices = canteen_window_indices[selected];
    int min_q = 1e9;
    vector<int> best;
    for (int idx : indices) {
        int q = (int)windows[idx].waiting_queue.size();
        if (q < min_q) { min_q = q; best.clear(); best.push_back(idx); }
        else if (q == min_q) best.push_back(idx);
    }
    uniform_int_distribution<int> dis(0, (int)best.size()-1);
    return best[dis(rng)];
}

// ---------- 策略决策（参数微调） ----------
int make_strategy_decision(double now) {
    int Wcnt = (int)windows.size();
    vector<double> W(Wcnt), t(Wcnt);
    vector<int> N(Wcnt);
    for (int i = 0; i < Wcnt; ++i) {
        W[i] = get_wait_time(i, now);
        t[i] = windows[i].t_sec;
        N[i] = (int)windows[i].waiting_queue.size();
    }

    // 剔除条件
    vector<int> candidates;
    for (int i = 0; i < Wcnt; ++i) {
        if (t[i] > 45 && N[i] >= 1) continue;
        if (t[i] > 16 && N[i] > 3) continue;
        candidates.push_back(i);
    }

    if ((int)candidates.size() < 4) {
        candidates.clear();
        vector<pair<double, int>> speed_sorted;
        for (int i = 0; i < Wcnt; ++i) speed_sorted.push_back({t[i], i});
        sort(speed_sorted.begin(), speed_sorted.end());
        int take = min(6, Wcnt);
        for (int i = 0; i < take; ++i) candidates.push_back(speed_sorted[i].second);
    }

    // 重新获取最新信息
    for (int i = 0; i < Wcnt; ++i) {
        W[i] = get_wait_time(i, now);
        N[i] = (int)windows[i].waiting_queue.size();
    }
    vector<int> candidates2;
    for (int idx : candidates) {
        if (t[idx] > 45 && N[idx] >= 1) continue;
        if (t[idx] > 16 && N[idx] > 3) continue;
        candidates2.push_back(idx);
    }
    if ((int)candidates2.size() < 4) candidates2 = candidates;

    // 计算Score（系数0.75）
    vector<pair<double, int>> scored;
    for (int idx : candidates2) {
        double score = W[idx] - 0.75 * (90.0 - t[idx]) / 60.0;
        scored.push_back({score, idx});
    }
    sort(scored.begin(), scored.end());

    int pick;
    if ((int)scored.size() >= 4) {
        int r = rand_0_9();
        int rank;
        if (r <= 2) rank = 2;       // 30%
        else if (r <= 5) rank = 3;  // 40%
        else rank = 4;              // 30%
        pick = scored[rank-1].second;
    } else if ((int)scored.size() >= 2) {
        pick = scored[1].second;
    } else {
        pick = scored[0].second;
    }
    return pick;
}

// ========================= 入队与服务 =========================

void enqueue_student(int student_id, int win_idx, double now) {
    Student& stu = students[student_id];
    stu.chosen_window = win_idx;
    stu.enqueue_time = now;
    windows[win_idx].waiting_queue.push(student_id);

    Window& w = windows[win_idx];
    if (w.waiting_queue.empty()) {
        w.service_end_time = now;
        return;
    }
    if (w.service_end_time > now + 1e-9) return;

    int sid = w.waiting_queue.front();
    w.waiting_queue.pop();
    Student& s = students[sid];
    s.service_start = now;
    s.wait_time = s.service_start - s.enqueue_time;
    w.service_end_time = now + w.t_min;
    served_count++;

    Event e;
    e.time = w.service_end_time;
    e.type = SERVICE_DONE;
    e.window_idx = win_idx;
    event_queue.push(e);
}

void process_service_done(int win_idx, double now) {
    Window& w = windows[win_idx];
    if (!w.waiting_queue.empty()) {
        int sid = w.waiting_queue.front();
        w.waiting_queue.pop();
        Student& s = students[sid];
        s.service_start = now;
        s.wait_time = s.service_start - s.enqueue_time;
        w.service_end_time = now + w.t_min;
        served_count++;
        Event e;
        e.time = w.service_end_time;
        e.type = SERVICE_DONE;
        e.window_idx = win_idx;
        event_queue.push(e);
    } else {
        w.service_end_time = now;
    }
}

void process_output(double now) {
    int total = 0;
    for (auto& w : windows) total += (int)w.waiting_queue.size();
    cout << fixed << setprecision(2) << now << " " << total << endl;
    if (served_count == TOTAL_STUDENTS) {
        simulation_done = true;
        return;
    }
    Event e;
    e.time = now + OUTPUT_INTERVAL;
    e.type = OUTPUT;
    event_queue.push(e);
}

// ========================= 主函数 =========================
int main() {
    init_windows();

    int flag;
    cout << "Enter flag (0=control, 1=experiment): ";
    cin >> flag;

    // 生成策略使用标记
    vector<int> use_group1(GROUP1_COUNT, 0);
    vector<int> use_group2(GROUP2_COUNT, 0);
    for (int i = 0; i < 1361; ++i) use_group1[i] = 1;
    for (int i = 0; i < 1658; ++i) use_group2[i] = 1;
    shuffle(use_group1.begin(), use_group1.end(), rng);
    shuffle(use_group2.begin(), use_group2.end(), rng);

    for (int i = 0; i < TOTAL_STUDENTS; ++i) {
        Student s;
        s.id = i;
        double release = (i < GROUP1_COUNT) ? GROUP1_RELEASE : GROUP2_RELEASE;
        int delay = rand_delay();
        s.ready_time = release + delay;
        s.walk_time = rand_walk();
        s.enqueue_time = 0;
        s.service_start = 0;
        s.wait_time = 0;

        bool use = false;
        if (i < GROUP1_COUNT) use = (use_group1[i] == 1);
        else {
            int idx = i - GROUP1_COUNT;
            use = (use_group2[idx] == 1);
        }
        s.use_strategy = use;

        if (flag == 0) {
            s.is_control = true;
            s.is_exp_use = false;
            s.is_exp_notuse = false;
            s.use_strategy = false;
        } else {
            s.is_control = false;
            s.is_exp_use = s.use_strategy;
            s.is_exp_notuse = !s.use_strategy;
        }
        s.chosen_window = -1;
        students[i] = s;
    }

    for (int i = 0; i < TOTAL_STUDENTS; ++i) {
        Event e;
        e.time = students[i].ready_time;
        e.type = ARRIVAL;
        e.student_id = i;
        e.window_idx = -1;
        event_queue.push(e);
    }
    Event first_out;
    first_out.time = OUTPUT_INTERVAL;
    first_out.type = OUTPUT;
    event_queue.push(first_out);

    while (!event_queue.empty() && !simulation_done) {
        Event e = event_queue.top();
        event_queue.pop();
        current_time = e.time;

        switch (e.type) {
            case ARRIVAL: {
                int sid = e.student_id;
                Student& stu = students[sid];
                if (stu.use_strategy) {
                    int total_Q = 0;
                    for (auto& w : windows) total_Q += (int)w.waiting_queue.size();
                    double delta = get_wait_time_by_congestion(total_Q);
                    Event dec;
                    dec.time = current_time + delta;
                    dec.type = STRATEGY_DECISION;
                    dec.student_id = sid;
                    dec.window_idx = -1;
                    event_queue.push(dec);
                } else {
                    Event enq;
                    enq.time = current_time + stu.walk_time;
                    enq.type = ENQUEUE;
                    enq.student_id = sid;
                    enq.window_idx = -1;
                    event_queue.push(enq);
                }
                break;
            }
            case STRATEGY_DECISION: {
                int sid = e.student_id;
                Student& stu = students[sid];
                int win = make_strategy_decision(current_time);
                Event enq;
                enq.time = current_time + stu.walk_time;
                enq.type = ENQUEUE;
                enq.student_id = sid;
                enq.window_idx = win;
                event_queue.push(enq);
                break;
            }
            case ENQUEUE: {
                int sid = e.student_id;
                int win_idx = e.window_idx;
                if (win_idx == -1) {
                    win_idx = select_window_non_strategy();
                }
                enqueue_student(sid, win_idx, current_time);
                break;
            }
            case SERVICE_DONE:
                process_service_done(e.window_idx, current_time);
                break;
            case OUTPUT:
                process_output(current_time);
                break;
        }
    }

    double total_wait = 0.0;
    double max_control = 0.0, max_exp_use = 0.0, max_exp_notuse = 0.0;
    int cnt_control = 0, cnt_exp_use = 0, cnt_exp_notuse = 0;

    for (const auto& s : students) {
        total_wait += s.wait_time;
        if (s.is_control) {
            cnt_control++;
            max_control = max(max_control, s.wait_time);
        }
        if (s.is_exp_use) {
            cnt_exp_use++;
            max_exp_use = max(max_exp_use, s.wait_time);
        }
        if (s.is_exp_notuse) {
            cnt_exp_notuse++;
            max_exp_notuse = max(max_exp_notuse, s.wait_time);
        }
    }

    double avg_wait = total_wait / TOTAL_STUDENTS;
    cout << "\n========== Simulation Results ==========\n";
    cout << "Average queue waiting time (all students): " << fixed << setprecision(2) << avg_wait << " minutes\n";
    if (flag == 0) {
        cout << "Control group max queue wait: " << max_control << " minutes\n";
        cout << "Experiment group (use strategy) max queue wait: N/A\n";
        cout << "Experiment group (not use) max queue wait: N/A\n";
    } else {
        cout << "Control group max queue wait: N/A\n";
        cout << "Experiment group (use strategy) max queue wait: " << max_exp_use << " minutes\n";
        cout << "Experiment group (not use) max queue wait: " << max_exp_notuse << " minutes\n";
    }

    return 0;
}
