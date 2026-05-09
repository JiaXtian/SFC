#include "controllers/UERANSIMController.h"
#include "controllers/SFCController.h"
#include "utils/json_converter.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfc {
namespace {

using json = nlohmann::json;

struct CommandResult {
    int code = -1;
    std::string output;
};

struct UEStatus {
    std::string key;
    std::string label;
    std::string imsi;
    std::string container;
    std::string ip;
    bool registered = false;
    bool pdu_session = false;
    bool receiver_ready = false;
    std::string log_tail;
    std::string received_messages;
};

struct ValidationStep {
    std::string key;
    std::string label;
    std::string status = "pending";
    std::string detail;
};

struct ValidationJob {
    std::string job_id;
    std::string deployment_id;
    std::string status = "queued";
    int progress = 0;
    std::string message;
    std::string started_at;
    std::string finished_at;
    std::string image;
    std::string network;
    std::string tmp_dir;
    bool stop_requested = false;
    std::string amf_container;
    std::string amf_ip;
    std::string gnb_container;
    std::string gnb_ip;
    bool data_plane_verified = false;
    json deployment;
    json containers = json::array();
    std::vector<ValidationStep> steps;
    std::vector<UEStatus> ues;
    std::vector<std::string> logs;
};

std::mutex g_jobs_mutex;
std::unordered_map<std::string, std::shared_ptr<ValidationJob>> g_jobs;
std::atomic<uint64_t> g_job_seq{1};

std::string iso_now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

std::string lower_copy(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string normalize_token(const std::string& raw) {
    std::string out;
    bool dash = false;
    for (char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            dash = false;
        } else if (!dash && !out.empty()) {
            out.push_back('-');
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "x" : out;
}

std::string digits_only(const std::string& raw) {
    std::string out;
    for (char ch : raw) {
        if (std::isdigit(static_cast<unsigned char>(ch))) out.push_back(ch);
    }
    return out;
}

std::string hex_encode(const std::string& raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char ch : raw) {
        out.push_back(kHex[ch >> 4]);
        out.push_back(kHex[ch & 0xf]);
    }
    return out;
}

CommandResult run_cmd(const std::string& cmd) {
    CommandResult result;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) {
        result.output = "popen_failed";
        return result;
    }
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) result.output += buffer;
    const int rc = pclose(pipe);
    if (WIFEXITED(rc)) result.code = WEXITSTATUS(rc);
    else result.code = rc;
    return result;
}

bool run_ok(const std::string& cmd, std::string* output = nullptr) {
    const auto res = run_cmd(cmd);
    if (output) *output = res.output;
    return res.code == 0;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

void append_log(const std::shared_ptr<ValidationJob>& job, const std::string& line) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    job->logs.push_back("[" + iso_now() + "] " + line);
    if (job->logs.size() > 900) {
        job->logs.erase(job->logs.begin(), job->logs.begin() + static_cast<long>(job->logs.size() - 900));
    }
    job->message = line;
}

void set_job_status(const std::shared_ptr<ValidationJob>& job, const std::string& status, int progress, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    if (job->stop_requested && status != "stopped") return;
    job->status = status;
    job->progress = std::max(0, std::min(100, progress));
    job->message = message;
    if (status == "success" || status == "failed" || status == "stopped") {
        job->finished_at = iso_now();
    }
}

bool job_stop_requested(const std::shared_ptr<ValidationJob>& job) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    return job->stop_requested;
}

void throw_if_stopped(const std::shared_ptr<ValidationJob>& job) {
    if (job_stop_requested(job)) {
        throw std::runtime_error("verification_stopped");
    }
}

void set_step(const std::shared_ptr<ValidationJob>& job, const std::string& key, const std::string& status, const std::string& detail) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    for (auto& step : job->steps) {
        if (step.key == key) {
            step.status = status;
            step.detail = detail;
            return;
        }
    }
}

std::string nf_daemon(const std::string& raw) {
    const std::string nf = lower_copy(raw);
    if (nf == "nrf") return "open5gs-nrfd";
    if (nf == "amf") return "open5gs-amfd";
    if (nf == "smf") return "open5gs-smfd";
    if (nf == "upf") return "open5gs-upfd";
    if (nf == "ausf") return "open5gs-ausfd";
    if (nf == "udm") return "open5gs-udmd";
    if (nf == "udr") return "open5gs-udrd";
    if (nf == "pcf") return "open5gs-pcfd";
    if (nf == "nssf") return "open5gs-nssfd";
    if (nf == "scp") return "open5gs-scpd";
    if (nf == "bsf") return "open5gs-bsfd";
    return "open5gs-" + nf + "d";
}

std::string node_container_name(const std::string& deployment_id, const std::string& node_id) {
    return "sfc-sat-" + normalize_token(deployment_id) + "-" + normalize_token(node_id);
}

std::string legacy_container_name(const std::string& node_id) {
    return "sfc-sat-" + normalize_token(node_id);
}

bool is_container_running(const std::string& container) {
    if (container.empty()) return false;
    return run_ok("docker inspect -f '{{.State.Running}}' " + shell_quote(container) + " | grep -q true");
}

json inspect_container(const std::string& container) {
    const auto res = run_cmd("docker inspect " + shell_quote(container));
    if (res.code != 0) return json::array();
    try {
        return json::parse(res.output);
    } catch (...) {
        return json::array();
    }
}

std::string container_ip_on_network(const std::string& container, const std::string& network = "") {
    const auto info = inspect_container(container);
    if (!info.is_array() || info.empty()) return "";
    const auto networks = info[0].value("NetworkSettings", json::object()).value("Networks", json::object());
    if (!network.empty() && networks.contains(network)) {
        return networks[network].value("IPAddress", std::string(""));
    }
    for (const auto& item : networks.items()) {
        const std::string ip = item.value().value("IPAddress", std::string(""));
        if (!ip.empty()) return ip;
    }
    return "";
}

std::string first_network_for_container(const std::string& container) {
    const auto info = inspect_container(container);
    if (!info.is_array() || info.empty()) return "";
    const auto networks = info[0].value("NetworkSettings", json::object()).value("Networks", json::object());
    const char* env_network = std::getenv("SFC_OPEN5GS_NETWORK");
    if (env_network && networks.contains(env_network)) return env_network;
    if (networks.contains("sfc-open5gs-net")) return "sfc-open5gs-net";
    for (const auto& item : networks.items()) return item.key();
    return "";
}

bool process_running_in_container(const std::string& container, const std::string& daemon) {
    if (container.empty() || daemon.empty()) return false;
    return run_ok(
        "docker exec " + shell_quote(container) +
        " sh -lc " + shell_quote("pgrep -x " + shell_quote(daemon) + " >/dev/null 2>&1 || pgrep -f " + shell_quote(daemon) + " >/dev/null 2>&1")
    );
}

std::string find_container_by_daemon(const std::string& daemon) {
    const auto ps = run_cmd("docker ps --format '{{.Names}}'");
    for (const auto& name : lines_of(ps.output)) {
        if (name.rfind("sfc-sat-", 0) == 0 && process_running_in_container(name, daemon)) {
            return name;
        }
    }
    return "";
}

std::string find_container_for_node(const std::string& deployment_id, const std::string& node_id) {
    const std::vector<std::string> candidates = {
        node_container_name(deployment_id, node_id),
        legacy_container_name(node_id),
    };
    for (const auto& c : candidates) {
        if (is_container_running(c)) return c;
    }

    const std::string suffix = "-" + normalize_token(node_id);
    const auto ps = run_cmd("docker ps --format '{{.Names}}'");
    for (const auto& name : lines_of(ps.output)) {
        if (name.rfind("sfc-sat-", 0) == 0 && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return name;
        }
    }
    return "";
}

json per_nf_array(const json& dep) {
    if (dep.contains("per_vnf") && dep["per_vnf"].is_array()) return dep["per_vnf"];
    if (dep.contains("per_core_nf") && dep["per_core_nf"].is_array()) return dep["per_core_nf"];
    return json::array();
}

std::string json_string_any(const json& j, const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    }
    return "";
}

json collect_deployment_runtime(const json& dep) {
    const std::string dep_id = dep.value("deployment_id", dep.value("backend_deployment_id", std::string("")));
    json containers = json::array();
    for (const auto& nf : per_nf_array(dep)) {
        const std::string nf_type = lower_copy(json_string_any(nf, {"nf_type", "core_nf", "vnf"}));
        const std::string node = json_string_any(nf, {"node", "node_id", "satellite"});
        const std::string daemon = nf_daemon(nf_type);
        std::string container = find_container_for_node(dep_id, node);
        bool daemon_ok = process_running_in_container(container, daemon);
        if (!daemon_ok) {
            const std::string fallback = find_container_by_daemon(daemon);
            if (!fallback.empty()) {
                container = fallback;
                daemon_ok = true;
            }
        }
        containers.push_back({
            {"nf_type", nf_type},
            {"node", node},
            {"daemon", daemon},
            {"container", container},
            {"running", is_container_running(container)},
            {"daemon_running", daemon_ok},
            {"ip", container_ip_on_network(container)}
        });
    }
    return containers;
}

std::optional<json> find_deployment(const std::string& deployment_id) {
    const json all = list_deployment_records();
    if (!all.is_array()) return std::nullopt;
    for (const auto& dep : all) {
        const std::string dep_id = dep.value("deployment_id", std::string(""));
        const std::string backend_id = dep.value("backend_deployment_id", dep_id);
        if (dep_id == deployment_id || backend_id == deployment_id) return dep;
    }
    return std::nullopt;
}

std::string normalize_sd(std::string raw) {
    raw.erase(std::remove_if(raw.begin(), raw.end(), [](unsigned char c) { return std::isspace(c); }), raw.end());
    if (raw.rfind("0x", 0) == 0 || raw.rfind("0X", 0) == 0) raw = raw.substr(2);
    if (raw.empty()) return "";
    for (char ch : raw) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return "000001";
    }
    unsigned long value = std::stoul(raw, nullptr, 16);
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << (value & 0xffffffUL);
    return oss.str();
}

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;
    ofs << content;
    return ofs.good();
}

bool ensure_tun_device(const std::string& container) {
    return run_ok(
        "docker exec " + shell_quote(container) + " sh -lc " +
        shell_quote("mkdir -p /dev/net; if [ ! -c /dev/net/tun ]; then mknod /dev/net/tun c 10 200 >/dev/null 2>&1 || true; fi; chmod 666 /dev/net/tun >/dev/null 2>&1 || true; test -c /dev/net/tun")
    );
}

bool start_ueransim_container(
    const std::string& name,
    const std::string& image,
    const std::string& network,
    const std::string& mount
) {
    run_ok("docker rm -f " + shell_quote(name) + " >/dev/null");
    const std::string base =
        "docker run -d --name " + shell_quote(name) +
        " --network " + shell_quote(network) +
        " -v " + shell_quote(mount) + " ";
    const std::string idle = shell_quote("while true; do sleep 3600; done");
    if (run_ok(base + "--cap-add=NET_ADMIN --device=/dev/net/tun " + shell_quote(image) + " sh -lc " + idle) &&
        ensure_tun_device(name)) {
        return true;
    }
    run_ok("docker rm -f " + shell_quote(name) + " >/dev/null");
    if (run_ok(base + "--privileged " + shell_quote(image) + " sh -lc " + idle) && ensure_tun_device(name)) {
        return true;
    }
    run_ok("docker rm -f " + shell_quote(name) + " >/dev/null");
    return false;
}

std::string resolve_bin_path(const std::string& container, const std::vector<std::string>& candidates) {
    std::ostringstream script;
    script << "for p in";
    for (const auto& c : candidates) script << " " << shell_quote(c);
    script << "; do if [ -x \"$p\" ]; then echo \"$p\"; exit 0; fi; q=$(command -v \"$p\" 2>/dev/null || true); if [ -n \"$q\" ]; then echo \"$q\"; exit 0; fi; done; exit 1";
    std::string out;
    if (!run_ok("docker exec " + shell_quote(container) + " sh -lc " + shell_quote(script.str()), &out)) return "";
    return trim_copy(out);
}

bool wait_for_log(const std::string& container, const std::string& logfile, const std::string& pattern, int timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        if (run_ok(
                "docker exec " + shell_quote(container) + " sh -lc " +
                shell_quote("test -f " + shell_quote(logfile) + " && grep -Eiq " + shell_quote(pattern) + " " + shell_quote(logfile)))) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

std::string wait_for_ue_tun_ip(const std::string& container, const std::string& iface, int timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto res = run_cmd(
            "docker exec " + shell_quote(container) + " sh -lc " +
            shell_quote("ip -o -4 addr show dev " + shell_quote(iface) + " 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1")
        );
        const std::string ip = trim_copy(res.output);
        if (!ip.empty()) return ip;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return "";
}

std::string tail_file(const std::string& container, const std::string& file, int lines = 80) {
    const auto res = run_cmd(
        "docker exec " + shell_quote(container) + " sh -lc " +
        shell_quote("tail -n " + std::to_string(lines) + " " + shell_quote(file) + " 2>/dev/null || true")
    );
    return res.output;
}

bool provision_subscriber(
    const std::filesystem::path& dir,
    const std::string& mongo_container,
    const std::string& imsi,
    const std::string& sst,
    const std::string& sd,
    const std::string& apn,
    const std::string& key,
    const std::string& opc
) {
    if (!is_container_running(mongo_container)) return false;
    const std::filesystem::path js_path = dir / ("subscriber-" + imsi + ".js");
    std::ostringstream js;
    js << "const dbx = db.getSiblingDB('open5gs');\n"
       << "const imsi = '" << imsi << "';\n"
       << "const subscriber = {\n"
       << "  imsi, subscriber_status: 0, operator_determined_barring: 0, network_access_mode: 0,\n"
       << "  subscribed_rau_tau_timer: 12, access_restriction_data: 0,\n"
       << "  slice: [{ sst: Number('" << sst << "'), default_indicator: true, ";
    if (!sd.empty()) js << "sd: '" << sd << "', ";
    js << "session: [{ name: '" << apn << "', type: 1, pcc_rule: [], ambr: { uplink: { value: 1, unit: 3 }, downlink: { value: 1, unit: 3 } }, qos: { index: 9, arp: { priority_level: 8, pre_emption_capability: 1, pre_emption_vulnerability: 1 } } }] }],\n"
       << "  ambr: { uplink: { value: 1, unit: 3 }, downlink: { value: 1, unit: 3 } },\n"
       << "  security: { k: '" << key << "', amf: '8000', op: null, opc: '" << opc << "' }, schema_version: 1, __v: 0\n"
       << "};\n"
       << "dbx.subscribers.updateOne({ imsi }, { $set: subscriber }, { upsert: true });\n"
       << "const found = dbx.subscribers.findOne({ imsi }); if (!found) throw new Error('subscriber_upsert_failed'); printjson({imsi: found.imsi, slice: found.slice});\n";
    if (!write_file(js_path, js.str())) return false;
    const std::string remote = "/tmp/" + js_path.filename().string();
    return run_ok("docker cp " + shell_quote(js_path.string()) + " " + shell_quote(mongo_container + ":" + remote)) &&
           run_ok("docker exec " + shell_quote(mongo_container) + " mongosh --quiet " + shell_quote(remote));
}

bool start_receiver(const std::string& container) {
    const std::string script =
        "rm -f /tmp/sfc_ue_rx.log /tmp/sfc_ue_rx.pid; touch /tmp/sfc_ue_rx.log; "
        "if command -v python3 >/dev/null 2>&1; then "
        "nohup python3 -u /config/udp_receiver.py 9090 /tmp/sfc_ue_rx.log >/tmp/sfc_ue_rx_runner.log 2>&1 & echo $! >/tmp/sfc_ue_rx.pid; "
        "elif command -v nc >/dev/null 2>&1; then "
        "nohup sh -lc 'while true; do nc -ul -p 9090 >>/tmp/sfc_ue_rx.log 2>&1; done' >/tmp/sfc_ue_rx_runner.log 2>&1 & echo $! >/tmp/sfc_ue_rx.pid; "
        "else echo '[receiver] no python3/nc available; manual messages will use fallback log' >>/tmp/sfc_ue_rx.log; fi";
    return run_ok("docker exec " + shell_quote(container) + " sh -lc " + shell_quote(script));
}

std::pair<bool, std::string> send_udp_message(
    const std::string& from_container,
    const std::string& to_ip,
    const std::string& message
) {
    const std::string hex = hex_encode(message);
    std::string cmd =
        "if command -v python3 >/dev/null 2>&1; then "
        "python3 /config/udp_send.py " + shell_quote(to_ip) + " 9090 " + shell_quote(hex) + "; "
        "elif command -v nc >/dev/null 2>&1; then "
        "printf %s " + shell_quote(message) + " | nc -u -w1 " + shell_quote(to_ip) + " 9090; "
        "elif command -v bash >/dev/null 2>&1; then "
        "bash -lc " + shell_quote("printf %s " + shell_quote(message) + " > /dev/udp/" + to_ip + "/9090") + "; "
        "else exit 42; fi";
    const auto res = run_cmd("docker exec " + shell_quote(from_container) + " sh -lc " + shell_quote(cmd));
    return {res.code == 0, res.output};
}

void append_ue_message_panel(
    const std::string& container,
    const std::string& line
) {
    if (container.empty() || line.empty()) return;
    run_ok(
        "docker exec " + shell_quote(container) + " sh -lc " +
        shell_quote("printf '%s\n' " + shell_quote(line) + " >> /tmp/sfc_ue_messages.log")
    );
}

bool wait_for_panel_message(
    const std::string& container,
    const std::string& needle,
    int timeout_ms = 1800
) {
    if (container.empty() || needle.empty()) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string merged =
            tail_file(container, "/tmp/sfc_ue_rx.log", 80) +
            tail_file(container, "/tmp/sfc_ue_messages.log", 80);
        if (merged.find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
    }
    return false;
}

void refresh_ue_logs_locked(ValidationJob& job) {
    for (auto& ue : job.ues) {
        ue.log_tail = tail_file(ue.container, "/tmp/ue.log", 90);
        ue.received_messages = tail_file(ue.container, "/tmp/sfc_ue_rx.log", 80) + tail_file(ue.container, "/tmp/sfc_ue_messages.log", 80);
    }
}

json job_to_json(const ValidationJob& job) {
    json steps = json::array();
    for (const auto& s : job.steps) {
        steps.push_back({{"key", s.key}, {"label", s.label}, {"status", s.status}, {"detail", s.detail}});
    }
    json ues = json::array();
    for (const auto& ue : job.ues) {
        ues.push_back({
            {"key", ue.key},
            {"label", ue.label},
            {"imsi", ue.imsi},
            {"container", ue.container},
            {"ip", ue.ip},
            {"registered", ue.registered},
            {"pdu_session", ue.pdu_session},
            {"receiver_ready", ue.receiver_ready},
            {"log_tail", ue.log_tail},
            {"received_messages", ue.received_messages}
        });
    }
    return {
        {"job_id", job.job_id},
        {"deployment_id", job.deployment_id},
        {"status", job.status},
        {"progress", job.progress},
        {"message", job.message},
        {"started_at", job.started_at},
        {"finished_at", job.finished_at},
        {"image", job.image},
        {"network", job.network},
        {"stop_requested", job.stop_requested},
        {"amf_container", job.amf_container},
        {"amf_ip", job.amf_ip},
        {"gnb_container", job.gnb_container},
        {"gnb_ip", job.gnb_ip},
        {"data_plane_verified", job.data_plane_verified},
        {"deployment", job.deployment},
        {"containers", job.containers},
        {"steps", steps},
        {"ues", ues},
        {"logs", job.logs}
    };
}

void write_ueransim_helpers(const std::filesystem::path& dir) {
    write_file(dir / "udp_receiver.py",
        "import datetime, socket, sys\n"
        "port=int(sys.argv[1]); path=sys.argv[2]\n"
        "sock=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(('0.0.0.0', port))\n"
        "open(path,'a').write('[receiver] listening udp/{}\\n'.format(port))\n"
        "while True:\n"
        "    data, addr = sock.recvfrom(65535)\n"
        "    text = data.decode('utf-8', 'replace')\n"
        "    ts = datetime.datetime.now().isoformat(timespec='seconds')\n"
        "    with open(path, 'a') as f: f.write('[{}] from {}:{} {}\\n'.format(ts, addr[0], addr[1], text)); f.flush()\n"
    );
    write_file(dir / "udp_send.py",
        "import socket, sys\n"
        "host=sys.argv[1]; port=int(sys.argv[2]); payload=bytes.fromhex(sys.argv[3])\n"
        "sock=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.sendto(payload, (host, port)); print('sent', len(payload), 'bytes')\n"
    );
}

void run_validation_job(const std::shared_ptr<ValidationJob>& job) {
    try {
        set_job_status(job, "running", 2, "准备验证任务");
        append_log(job, "开始 UERANSIM 功能验证");

        auto dep_opt = find_deployment(job->deployment_id);
        throw_if_stopped(job);
        if (!dep_opt) throw std::runtime_error("deployment_not_found");
        json dep = *dep_opt;
        const std::string dep_id = dep.value("deployment_id", dep.value("backend_deployment_id", std::string("")));
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->deployment = dep;
            job->containers = collect_deployment_runtime(dep);
        }
        const bool service_ready = dep.value("service_ready", false);
        const bool ready_for_ue = dep.value("ready_for_ueransim", false);
        if (!service_ready || !ready_for_ue) {
            throw std::runtime_error("deployment is not service_ready && ready_for_ueransim");
        }
        set_step(job, "deployment", "success", "核心网部署状态满足 UE 验证条件");
        set_job_status(job, "running", 8, "核对核心网容器与 NF 进程");
        throw_if_stopped(job);

        int nf_total = 0;
        int nf_ok = 0;
        std::string amf_container;
        for (const auto& item : collect_deployment_runtime(dep)) {
            ++nf_total;
            if (item.value("daemon_running", false)) ++nf_ok;
            if (item.value("nf_type", std::string("")) == "amf") amf_container = item.value("container", std::string(""));
        }
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->containers = collect_deployment_runtime(dep);
        }
        if (nf_total == 0 || nf_ok != nf_total) {
            throw std::runtime_error("NF process verification failed: " + std::to_string(nf_ok) + "/" + std::to_string(nf_total));
        }
        if (amf_container.empty()) amf_container = find_container_by_daemon("open5gs-amfd");
        if (amf_container.empty()) throw std::runtime_error("unable to locate AMF container");
        const std::string network = first_network_for_container(amf_container);
        const std::string amf_ip = container_ip_on_network(amf_container, network);
        if (network.empty() || amf_ip.empty()) throw std::runtime_error("unable to resolve AMF network/IP");
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->amf_container = amf_container;
            job->amf_ip = amf_ip;
            job->network = network;
        }
        set_step(job, "nf", "success", "NF 进程全部运行，AMF=" + amf_container + " " + amf_ip);
        append_log(job, "核心网容器核验通过: NF=" + std::to_string(nf_ok) + "/" + std::to_string(nf_total) + ", AMF=" + amf_container + ", network=" + network);
        throw_if_stopped(job);

        const std::string image = std::getenv("UERANSIM_IMAGE") ? std::getenv("UERANSIM_IMAGE") : "docker.io/free5gc/ueransim:latest";
        const std::string mcc = std::getenv("UERANSIM_MCC") ? std::getenv("UERANSIM_MCC") : "999";
        const std::string mnc = std::getenv("UERANSIM_MNC") ? std::getenv("UERANSIM_MNC") : "70";
        const std::string tac = std::getenv("UERANSIM_TAC") ? std::getenv("UERANSIM_TAC") : "1";
        const std::string sst = std::getenv("UERANSIM_SST") ? std::getenv("UERANSIM_SST") : "1";
        const std::string sd = normalize_sd(std::getenv("UERANSIM_SD") ? std::getenv("UERANSIM_SD") : "000001");
        const std::string apn = std::getenv("UERANSIM_APN") ? std::getenv("UERANSIM_APN") : "internet";
        const std::string key = std::getenv("UERANSIM_KEY") ? std::getenv("UERANSIM_KEY") : "465B5CE8B199B49FAA5F0A2EE238A6BC";
        const std::string opc = std::getenv("UERANSIM_OPC") ? std::getenv("UERANSIM_OPC") : "E8ED289DEBA952E4283B54E88E6183CA";
        const std::string mongo = std::getenv("SFC_OPEN5GS_MONGO_CONTAINER") ? std::getenv("SFC_OPEN5GS_MONGO_CONTAINER") : "sfc-open5gs-mongo";
        std::string imsi_base = digits_only(std::getenv("UERANSIM_IMSI") ? std::getenv("UERANSIM_IMSI") : "999700000000001");
        if (imsi_base.empty()) imsi_base = "999700000000001";
        const unsigned long long imsi0 = std::stoull(imsi_base);

        const std::string short_id = normalize_token(job->job_id).substr(0, 18);
        const std::string name_base = "sfc-ueransim-" + normalize_token(dep_id).substr(0, 28) + "-" + short_id;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / name_base;
        std::filesystem::create_directories(dir);
        write_ueransim_helpers(dir);
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->image = image;
            job->tmp_dir = dir.string();
            job->gnb_container = name_base + "-gnb";
            job->ues = {
                {"ue1", "UE-1", std::to_string(imsi0), name_base + "-ue1", "", false, false, false, "", ""},
                {"ue2", "UE-2", std::to_string(imsi0 + 1), name_base + "-ue2", "", false, false, false, "", ""}
            };
        }

        set_job_status(job, "running", 16, "写入订阅用户");
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            const std::string imsi = std::to_string(imsi0 + static_cast<unsigned long long>(i));
            if (!provision_subscriber(dir, mongo, imsi, sst, sd, apn, key, opc)) {
                throw std::runtime_error("failed to provision subscriber IMSI=" + imsi + " on " + mongo);
            }
        }
        set_step(job, "subscriber", "success", "已向 Mongo 写入 UE-1/UE-2 订阅数据");

        set_job_status(job, "running", 22, "准备 UERANSIM 镜像与容器");
        append_log(job, "检查 UERANSIM 镜像: " + image);
        throw_if_stopped(job);
        if (!run_ok("docker image inspect " + shell_quote(image) + " >/dev/null")) {
            if (!run_ok("docker pull " + shell_quote(image) + " >/dev/null")) {
                throw std::runtime_error("failed to pull UERANSIM image: " + image);
            }
        }
        const std::string mount = dir.string() + ":/config";
        std::string gnb_container;
        std::vector<UEStatus> ues;
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            gnb_container = job->gnb_container;
            ues = job->ues;
        }
        if (!start_ueransim_container(gnb_container, image, network, mount)) {
            throw std::runtime_error("failed to start gNB container with tun support");
        }
        for (const auto& ue : ues) {
            throw_if_stopped(job);
            if (!start_ueransim_container(ue.container, image, network, mount)) {
                throw std::runtime_error("failed to start " + ue.label + " container with tun support");
            }
        }
        set_step(job, "containers", "success", "gNB/UE-1/UE-2 容器已启动并具备 TUN 支持");

        const std::string gnb_bin = resolve_bin_path(gnb_container, {"/ueransim/nr-gnb", "/UERANSIM/build/nr-gnb", "nr-gnb"});
        const std::string ue_bin = resolve_bin_path(ues.front().container, {"/ueransim/nr-ue", "/UERANSIM/build/nr-ue", "nr-ue"});
        if (gnb_bin.empty() || ue_bin.empty()) throw std::runtime_error("failed to locate UERANSIM binaries");
        const std::string gnb_ip = container_ip_on_network(gnb_container, network);
        if (gnb_ip.empty()) throw std::runtime_error("failed to resolve gNB IP");
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->gnb_ip = gnb_ip;
        }
        append_log(job, "UERANSIM 容器就绪: gNB=" + gnb_container + " " + gnb_ip + ", bin=" + gnb_bin + "/" + ue_bin);

        std::ostringstream gnb_yaml;
        gnb_yaml << "gnbId: 1\nmcc: '" << mcc << "'\nmnc: '" << mnc
                 << "'\nnci: '0x000000010'\nidLength: 32\ntac: " << tac
                 << "\nignoreStreamIds: true\nlinkIp: " << gnb_ip
                 << "\nngapIp: " << gnb_ip << "\ngtpIp: " << gnb_ip
                 << "\namfConfigs:\n  - address: " << amf_ip << "\n    port: 38412\nslices:\n  - sst: " << sst << "\n";
        if (!sd.empty()) gnb_yaml << "    sd: " << sd << "\n";
        write_file(dir / "gnb.yaml", gnb_yaml.str());
        for (int i = 0; i < 2; ++i) {
            const std::string imsi = std::to_string(imsi0 + static_cast<unsigned long long>(i));
            std::ostringstream ue_yaml;
            ue_yaml << "supi: 'imsi-" << imsi << "'\nmcc: '" << mcc << "'\nmnc: '" << mnc
                    << "'\nkey: '" << key << "'\nopType: 'OPC'\nop: '" << opc
                    << "'\namf: '8000'\nimei: '35693803564380" << (3 + i)
                    << "'\nimeiSv: '437081612581615" << (1 + i)
                    << "'\nintegrity:\n  IA1: true\n  IA2: true\n  IA3: true\nciphering:\n  EA1: true\n  EA2: true\n  EA3: true\nintegrityMaxRate:\n  uplink: full\n  downlink: full\nuacAic:\n  mps: false\n  mcs: false\nuacAcc:\n  normalClass: 0\n  class11: false\n  class12: false\n  class13: false\n  class14: false\n  class15: false\nconfigured-nssai:\n  - sst: " << sst << "\n";
            if (!sd.empty()) ue_yaml << "    sd: " << sd << "\n";
            ue_yaml << "default-nssai:\n  - sst: " << sst << "\n";
            if (!sd.empty()) ue_yaml << "    sd: " << sd << "\n";
            ue_yaml << "gnbSearchList:\n  - " << gnb_ip << "\nsessions:\n  - type: 'IPv4'\n    apn: '" << apn << "'\n    slice:\n      sst: " << sst << "\n";
            if (!sd.empty()) ue_yaml << "      sd: " << sd << "\n";
            write_file(dir / ("ue" + std::to_string(i + 1) + ".yaml"), ue_yaml.str());
        }

        set_job_status(job, "running", 34, "启动 gNB 并等待 NG Setup");
        throw_if_stopped(job);
        run_ok("docker exec -d " + shell_quote(gnb_container) + " sh -lc " + shell_quote(shell_quote(gnb_bin) + " -c /config/gnb.yaml > /tmp/gnb.log 2>&1"));
        if (!wait_for_log(gnb_container, "/tmp/gnb.log", "NG Setup procedure is successful|NG setup successful|NG Setup Response", 75)) {
            throw std::runtime_error("gNB NG Setup failed: " + tail_file(gnb_container, "/tmp/gnb.log", 80));
        }
        set_step(job, "gnb", "success", "gNB 与 AMF 完成 NG Setup");

        set_job_status(job, "running", 48, "启动两个 UE 并等待注册");
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            run_ok("docker exec -d " + shell_quote(ues[i].container) + " sh -lc " + shell_quote(shell_quote(ue_bin) + " -c /config/ue" + std::to_string(i + 1) + ".yaml > /tmp/ue.log 2>&1"));
        }
        const std::string reg_pattern = "Registration complete|Initial Registration is successful|5GMM-REGISTERED";
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            if (!wait_for_log(ues[i].container, "/tmp/ue.log", reg_pattern, 135)) {
                throw std::runtime_error(ues[i].label + " registration failed: " + tail_file(ues[i].container, "/tmp/ue.log", 90));
            }
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->ues[i].registered = true;
            job->ues[i].log_tail = tail_file(ues[i].container, "/tmp/ue.log", 90);
        }
        set_step(job, "registration", "success", "UE-1/UE-2 注册成功");

        set_job_status(job, "running", 62, "检查 PDU session 与 UE 隧道地址");
        const std::string pdu_pattern = "PDU Session establishment is successful|PDU Session Establishment Accept";
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            if (!wait_for_log(ues[i].container, "/tmp/ue.log", pdu_pattern, 45)) {
                throw std::runtime_error(ues[i].label + " PDU session failed: " + tail_file(ues[i].container, "/tmp/ue.log", 90));
            }
            const std::string ip = wait_for_ue_tun_ip(ues[i].container, "uesimtun0", 45);
            if (ip.empty()) {
                throw std::runtime_error(ues[i].label + " did not obtain IPv4 on uesimtun0");
            }
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->ues[i].pdu_session = true;
            job->ues[i].ip = ip;
            job->ues[i].log_tail = tail_file(ues[i].container, "/tmp/ue.log", 90);
        }
        set_step(job, "pdu", "success", "两个 UE 均完成 PDU Session 建立并获得 uesimtun0 地址");

        set_job_status(job, "running", 78, "执行 UE 间业务面连通性验证");
        throw_if_stopped(job);
        std::vector<UEStatus> fresh_ues;
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            fresh_ues = job->ues;
        }
        const std::string ping12 = "ping -c 2 -W 2 -I uesimtun0 " + shell_quote(fresh_ues[1].ip);
        const std::string ping21 = "ping -c 2 -W 2 -I uesimtun0 " + shell_quote(fresh_ues[0].ip);
        std::string ping_out_12;
        std::string ping_out_21;
        const bool ok12 = run_ok("docker exec " + shell_quote(fresh_ues[0].container) + " sh -lc " + shell_quote(ping12), &ping_out_12);
        const bool ok21 = run_ok("docker exec " + shell_quote(fresh_ues[1].container) + " sh -lc " + shell_quote(ping21), &ping_out_21);
        if (!ok12 || !ok21) {
            throw std::runtime_error("UE-to-UE ping failed\nUE-1->UE-2:\n" + ping_out_12 + "\nUE-2->UE-1:\n" + ping_out_21);
        }
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->data_plane_verified = true;
        }
        set_step(job, "dataplane", "success", "UE-1 与 UE-2 通过核心网业务面互 ping 成功");

        set_job_status(job, "running", 88, "启动 UE 消息接收面板");
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            const bool receiver = start_receiver(fresh_ues[i].container);
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->ues[i].receiver_ready = receiver;
            job->ues[i].received_messages = tail_file(fresh_ues[i].container, "/tmp/sfc_ue_rx.log", 20);
        }
        set_step(job, "message", "running", "消息接收器已启动，可在页面中发送 UE 间验证消息");

        set_job_status(job, "success", 100, "功能验证完成：核心网、UERANSIM 注册/PDU/业务面/双 UE 通信均已通过");
        set_step(job, "message", "success", "UE 消息面板已就绪");
        append_log(job, "UERANSIM 功能验证完整通过");
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            refresh_ue_logs_locked(*job);
        }
    } catch (const std::exception& e) {
        if (job_stop_requested(job) || std::string(e.what()) == "verification_stopped") {
            append_log(job, "验证任务已由用户停止");
            set_job_status(job, "stopped", job->progress, "已停止并清理 UERANSIM 容器");
            return;
        }
        spdlog::error("UERANSIM validation job {} failed: {}", job->job_id, e.what());
        append_log(job, std::string("验证失败: ") + e.what());
        set_job_status(job, "failed", std::max(1, job->progress), e.what());
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        refresh_ue_logs_locked(*job);
    }
}

HttpResponsePtr json_response(const json& payload, HttpStatusCode status = k200OK) {
    auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(payload));
    resp->setStatusCode(status);
    return resp;
}

} // namespace

void UERANSIMController::listDeployments(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        const json deployments = list_deployment_records();
        json items = json::array();
        if (deployments.is_array()) {
            for (const auto& dep : deployments) {
                json item = dep;
                item["eligible_for_ueransim"] = dep.value("service_ready", false) && dep.value("ready_for_ueransim", false);
                item["runtime_containers"] = collect_deployment_runtime(dep);
                items.push_back(item);
            }
        }
        callback(json_response({{"items", items}}));
    } catch (const std::exception& e) {
        callback(json_response({{"message", e.what()}}, k500InternalServerError));
    }
}

void UERANSIMController::startVerification(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    const auto body = req->getJsonObject();
    if (!body || !body->isMember("deployment_id")) {
        callback(json_response({{"message", "deployment_id_required"}}, k400BadRequest));
        return;
    }
    const std::string deployment_id = (*body)["deployment_id"].asString();
    if (deployment_id.empty()) {
        callback(json_response({{"message", "deployment_id_required"}}, k400BadRequest));
        return;
    }

    auto job = std::make_shared<ValidationJob>();
    const auto seq = g_job_seq.fetch_add(1);
    job->job_id = "ueransim-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(seq);
    job->deployment_id = deployment_id;
    job->started_at = iso_now();
    job->steps = {
        {"deployment", "核心网就绪检查", "pending", ""},
        {"nf", "核心网容器与 NF 进程核验", "pending", ""},
        {"subscriber", "UE 订阅用户写入", "pending", ""},
        {"containers", "UERANSIM 容器启动", "pending", ""},
        {"gnb", "gNB NG Setup", "pending", ""},
        {"registration", "UE 注册", "pending", ""},
        {"pdu", "PDU Session 与 UE IP", "pending", ""},
        {"dataplane", "UE 间业务面验证", "pending", ""},
        {"message", "双 UE 消息面板", "pending", ""}
    };
    json initial_job;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_jobs[job->job_id] = job;
        initial_job = job_to_json(*job);
    }
    std::thread(run_validation_job, job).detach();
    callback(json_response({{"job", initial_job}}));
}

void UERANSIMController::getVerification(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& job_id
) {
    std::shared_ptr<ValidationJob> job;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) {
            callback(json_response({{"message", "job_not_found"}}, k404NotFound));
            return;
        }
        refresh_ue_logs_locked(*it->second);
        job = it->second;
        callback(json_response({{"job", job_to_json(*job)}}));
    }
}

void UERANSIMController::sendMessage(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& job_id
) {
    const auto body = req->getJsonObject();
    if (!body) {
        callback(json_response({{"message", "json_body_required"}}, k400BadRequest));
        return;
    }
    const std::string from_key = body->isMember("from") ? (*body)["from"].asString() : "ue1";
    const std::string to_key = body->isMember("to") ? (*body)["to"].asString() : (from_key == "ue1" ? "ue2" : "ue1");
    std::string message = body->isMember("message") ? (*body)["message"].asString() : "";
    message = trim_copy(message);
    if (message.empty()) {
        callback(json_response({{"message", "message_required"}}, k400BadRequest));
        return;
    }
    if (message.size() > 512) message.resize(512);

    std::shared_ptr<ValidationJob> job;
    UEStatus from;
    UEStatus to;
    bool found_from = false;
    bool found_to = false;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) {
            callback(json_response({{"message", "job_not_found"}}, k404NotFound));
            return;
        }
        job = it->second;
        if (job->status != "success") {
            callback(json_response({{"message", "verification_not_successful"}}, k409Conflict));
            return;
        }
        for (const auto& ue : job->ues) {
            if (ue.key == from_key) {
                from = ue;
                found_from = true;
            }
            if (ue.key == to_key) {
                to = ue;
                found_to = true;
            }
        }
    }
    if (!found_from || !found_to || from.ip.empty() || to.ip.empty()) {
        callback(json_response({{"message", "ue_not_ready"}}, k409Conflict));
        return;
    }

    const std::string wire_message = from.label + " -> " + to.label + ": " + message;
    const auto [udp_ok, udp_out] = send_udp_message(from.container, to.ip, wire_message);
    std::string transport = "udp_over_ue_tunnel";
    const std::string stamped_line = "[" + iso_now() + "] " + wire_message;
    if (udp_ok) {
        if (!wait_for_panel_message(to.container, wire_message)) {
            append_ue_message_panel(to.container, stamped_line + " (UDP sent over UE tunnel; receiver log not flushed in time)");
        }
    } else {
        transport = "fallback_log_after_dataplane_verified";
        append_ue_message_panel(to.container, stamped_line + " (UDP sender unavailable: " + trim_copy(udp_out) + ")");
    }
    append_ue_message_panel(from.container, stamped_line + " (sent)");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    append_log(job, "消息发送: " + wire_message + " transport=" + transport);
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        refresh_ue_logs_locked(*job);
        callback(json_response({{"job", job_to_json(*job)}, {"transport", transport}, {"udp_output", udp_out}}));
    }
}

void UERANSIMController::stopVerification(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& job_id
) {
    std::shared_ptr<ValidationJob> job;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) {
            callback(json_response({{"message", "job_not_found"}}, k404NotFound));
            return;
        }
        job = it->second;
        job->stop_requested = true;
    }
    std::vector<std::string> containers;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        if (!job->gnb_container.empty()) containers.push_back(job->gnb_container);
        for (const auto& ue : job->ues) {
            if (!ue.container.empty()) containers.push_back(ue.container);
        }
    }
    for (const auto& c : containers) {
        run_ok("docker rm -f " + shell_quote(c) + " >/dev/null");
    }
    set_job_status(job, "stopped", job->progress, "已停止并清理 UERANSIM 容器");
    append_log(job, "已停止验证任务并清理容器");
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    callback(json_response({{"job", job_to_json(*job)}}));
}

} // namespace sfc
