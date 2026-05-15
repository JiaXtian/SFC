#include "utils/Sgp4Propagator.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <stdexcept>
#include <sstream>

namespace sfc::sgp4 {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kEarthRadiusKm = 6378.135;          // WGS-72 equatorial radius used by SGP4
constexpr double kEarthMuKm3Sec2 = 398600.8;         // WGS-72 gravitational parameter
constexpr double kJ2 = 1.082616e-3;
constexpr double kMinutesPerDay = 1440.0;
constexpr double kSecondsPerDay = 86400.0;

double deg_to_rad(double v) { return v * kPi / 180.0; }
double rad_to_deg(double v) { return v * 180.0 / kPi; }

double wrap_deg(double v) {
    double out = std::fmod(v, 360.0);
    if (out < 0.0) out += 360.0;
    return out;
}

double clamp(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

double parse_double_field(const std::string& raw, const char* name) {
    const std::string value = trim(raw);
    if (value.empty()) throw std::runtime_error(std::string("missing TLE field: ") + name);
    return std::stod(value);
}

double julian_day_ymd(int year, int month, int day) {
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    const int a = year / 100;
    const int b = 2 - a + a / 4;
    return std::floor(365.25 * static_cast<double>(year + 4716))
        + std::floor(30.6001 * static_cast<double>(month + 1))
        + static_cast<double>(day) + static_cast<double>(b) - 1524.5;
}

double parse_tle_epoch_jd(const std::string& line1) {
    if (line1.size() < 32) throw std::runtime_error("TLE line1 is too short for epoch");
    const std::string epoch = trim(line1.substr(18, 14));
    if (epoch.size() < 5) throw std::runtime_error("invalid TLE epoch");
    const int yy = std::stoi(epoch.substr(0, 2));
    const int year = yy < 57 ? 2000 + yy : 1900 + yy;
    const double day_of_year = std::stod(epoch.substr(2));
    return julian_day_ymd(year, 1, 1) + (day_of_year - 1.0);
}

std::string iso_utc_from_julian(double epoch_jd) {
    const double unix_seconds = (epoch_jd - 2440587.5) * kSecondsPerDay;
    const auto whole = static_cast<std::time_t>(std::floor(unix_seconds));
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &whole);
#else
    gmtime_r(&whole, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

double parse_compact_tle_exponential(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) return 0.0;

    int sign = 1;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        sign = s[0] == '-' ? -1 : 1;
        s = s.substr(1);
    }
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
    if (s.size() < 3) return 0.0;
    const size_t exp_pos = s.find_last_of("+-");
    if (exp_pos == std::string::npos || exp_pos == 0 || exp_pos + 1 >= s.size()) return 0.0;
    const std::string mantissa_digits = s.substr(0, exp_pos);
    const int exp = std::stoi(s.substr(exp_pos));
    const double mantissa = static_cast<double>(std::stoll(mantissa_digits))
        / std::pow(10.0, static_cast<int>(mantissa_digits.size()));
    return static_cast<double>(sign) * mantissa * std::pow(10.0, exp);
}

double solve_kepler(double mean_anomaly_rad, double eccentricity) {
    double e = mean_anomaly_rad;
    for (int i = 0; i < 10; ++i) {
        const double f = e - eccentricity * std::sin(e) - mean_anomaly_rad;
        const double fp = 1.0 - eccentricity * std::cos(e);
        if (std::abs(fp) < 1e-12) break;
        const double step = f / fp;
        e -= step;
        if (std::abs(step) < 1e-12) break;
    }
    return e;
}

std::string format_tle_epoch(double epoch_jd) {
    const double unix_seconds = (epoch_jd - 2440587.5) * kSecondsPerDay;
    const auto whole = static_cast<std::time_t>(std::floor(unix_seconds));
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &whole);
#else
    gmtime_r(&whole, &tm);
#endif
    const int year = (tm.tm_year + 1900) % 100;
    const int doy = tm.tm_yday + 1;
    const double day_fraction = (unix_seconds - std::floor(unix_seconds)) / kSecondsPerDay;
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << year
        << std::setw(3) << doy
        << std::fixed << std::setprecision(8) << day_fraction;
    return oss.str();
}

std::string compact_tle_exponential(double value) {
    if (std::abs(value) < 1e-12) return " 00000-0";
    const int sign = value < 0.0 ? -1 : 1;
    double mant = std::abs(value);
    int exp = 0;
    while (mant < 0.1) {
        mant *= 10.0;
        exp -= 1;
    }
    while (mant >= 1.0) {
        mant /= 10.0;
        exp += 1;
    }
    const int mantissa = static_cast<int>(std::round(mant * 100000.0));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%05d%c%d", sign < 0 ? '-' : ' ', mantissa, exp < 0 ? '-' : '+', std::abs(exp));
    return std::string(buf);
}

std::string make_tle_line1(int satnum, double epoch_jd, double bstar) {
    char buf[96];
    std::snprintf(
        buf,
        sizeof(buf),
        "1 %05dU 26001A   %14s  .00000000  00000-0 %8s 0  9990",
        satnum % 100000,
        format_tle_epoch(epoch_jd).c_str(),
        compact_tle_exponential(bstar).c_str()
    );
    return std::string(buf);
}

std::string make_tle_line2(
    int satnum,
    double inclination_deg,
    double raan_deg,
    double eccentricity,
    double argp_deg,
    double mean_anomaly_deg,
    double mean_motion_rev_per_day
) {
    char buf[96];
    const int ecc7 = static_cast<int>(std::round(clamp(eccentricity, 0.0, 0.9999999) * 10000000.0));
    std::snprintf(
        buf,
        sizeof(buf),
        "2 %05d %8.4f %8.4f %07d %8.4f %8.4f %11.8f00000",
        satnum % 100000,
        inclination_deg,
        wrap_deg(raan_deg),
        ecc7,
        wrap_deg(argp_deg),
        wrap_deg(mean_anomaly_deg),
        mean_motion_rev_per_day
    );
    return std::string(buf);
}

} // namespace

double julian_date_from_unix(double unix_seconds) {
    return 2440587.5 + unix_seconds / kSecondsPerDay;
}

double julian_date_now() {
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = std::chrono::duration<double>(now.time_since_epoch()).count();
    return julian_date_from_unix(since_epoch);
}

std::string iso_utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

double mean_motion_from_altitude_rev_per_day(double altitude_km) {
    const double a = kEarthRadiusKm + std::max(100.0, altitude_km);
    const double n_rad_sec = std::sqrt(kEarthMuKm3Sec2 / (a * a * a));
    return n_rad_sec * kSecondsPerDay / kTwoPi;
}

double semi_major_axis_from_mean_motion_km(double mean_motion_rev_per_day) {
    const double n_rad_sec = std::max(1e-9, mean_motion_rev_per_day) * kTwoPi / kSecondsPerDay;
    return std::cbrt(kEarthMuKm3Sec2 / (n_rad_sec * n_rad_sec));
}

double orbital_period_minutes(double mean_motion_rev_per_day) {
    return kMinutesPerDay / std::max(1e-9, mean_motion_rev_per_day);
}

bool parse_tle_into_params(OrbitalParams& params, const std::string& tle_line1, const std::string& tle_line2, std::string* error) {
    try {
        const std::string line1 = trim(tle_line1);
        const std::string line2 = trim(tle_line2);
        if (line1.size() < 63 || line2.size() < 63) {
            throw std::runtime_error("TLE line length is incomplete");
        }
        if (line1[0] != '1' || line2[0] != '2') {
            throw std::runtime_error("TLE lines must start with 1/2");
        }

        params.propagation_model = "SGP4";
        params.tle_line1 = line1;
        params.tle_line2 = line2;
        params.epoch_jd = parse_tle_epoch_jd(line1);
        params.epoch_iso = iso_utc_from_julian(params.epoch_jd);
        params.bstar = line1.size() >= 61 ? parse_compact_tle_exponential(line1.substr(53, 8)) : params.bstar;

        params.inclination_deg = parse_double_field(line2.substr(8, 8), "inclination");
        params.raan = wrap_deg(parse_double_field(line2.substr(17, 8), "raan"));
        const std::string ecc_digits = trim(line2.substr(26, 7));
        params.eccentricity = ecc_digits.empty() ? 0.0 : std::stod("0." + ecc_digits);
        params.argument_of_perigee_deg = wrap_deg(parse_double_field(line2.substr(34, 8), "argument_of_perigee"));
        params.mean_anomaly_deg = wrap_deg(parse_double_field(line2.substr(43, 8), "mean_anomaly"));
        params.true_anomaly = params.mean_anomaly_deg;
        params.mean_motion_rev_per_day = parse_double_field(line2.substr(52, 11), "mean_motion");
        params.semi_major_axis_km = semi_major_axis_from_mean_motion_km(params.mean_motion_rev_per_day);
        params.period_minutes = orbital_period_minutes(params.mean_motion_rev_per_day);
        if (params.altitude_km <= 0.0 || params.altitude_km == 550.0) {
            params.altitude_km = params.semi_major_axis_km - kEarthRadiusKm;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

OrbitalParams make_walker_sgp4_params(
    int plane,
    int position_in_plane,
    int total_planes,
    int sats_per_plane,
    double altitude_km,
    double inclination_deg,
    int phasing,
    double epoch_jd
) {
    OrbitalParams params;
    params.propagation_model = "SGP4";
    params.plane = plane;
    params.position_in_plane = position_in_plane;
    params.raan = total_planes > 0 ? 360.0 * static_cast<double>(plane) / static_cast<double>(total_planes) : 0.0;
    const double phase = (total_planes > 0 && sats_per_plane > 0)
        ? 360.0 * static_cast<double>(plane * phasing)
            / static_cast<double>(std::max(1, total_planes * sats_per_plane))
        : 0.0;
    params.true_anomaly = sats_per_plane > 0
        ? wrap_deg(360.0 * static_cast<double>(position_in_plane) / static_cast<double>(sats_per_plane) + phase)
        : 0.0;
    params.altitude_km = altitude_km;
    params.inclination_deg = inclination_deg;
    params.eccentricity = 0.0001;
    params.argument_of_perigee_deg = 0.0;
    params.mean_anomaly_deg = params.true_anomaly;
    params.mean_motion_rev_per_day = mean_motion_from_altitude_rev_per_day(altitude_km);
    params.bstar = 0.00005;
    params.epoch_jd = epoch_jd > 0.0 ? epoch_jd : julian_date_now();
    params.epoch_iso = iso_utc_now();
    params.semi_major_axis_km = semi_major_axis_from_mean_motion_km(params.mean_motion_rev_per_day);
    params.period_minutes = orbital_period_minutes(params.mean_motion_rev_per_day);
    const int satnum = plane * 1000 + position_in_plane + 1;
    params.tle_line1 = make_tle_line1(satnum, params.epoch_jd, params.bstar);
    params.tle_line2 = make_tle_line2(
        satnum,
        params.inclination_deg,
        params.raan,
        params.eccentricity,
        params.argument_of_perigee_deg,
        params.mean_anomaly_deg,
        params.mean_motion_rev_per_day
    );
    return params;
}

void ensure_sgp4_defaults(OrbitalParams& params, double fallback_altitude_km, double fallback_inclination_deg) {
    if (params.propagation_model.empty()) params.propagation_model = "SGP4";
    if (!params.tle_line1.empty() || !params.tle_line2.empty()) {
        std::string parse_error;
        if (!parse_tle_into_params(params, params.tle_line1, params.tle_line2, &parse_error)) {
            throw std::runtime_error("Invalid SGP4 TLE: " + parse_error);
        }
    }
    if (params.altitude_km <= 0.0) params.altitude_km = fallback_altitude_km > 0.0 ? fallback_altitude_km : 550.0;
    if (params.inclination_deg == 0.0 && fallback_inclination_deg > 0.0) params.inclination_deg = fallback_inclination_deg;
    if (params.mean_motion_rev_per_day <= 0.0) {
        params.mean_motion_rev_per_day = mean_motion_from_altitude_rev_per_day(params.altitude_km);
    }
    if (params.eccentricity < 0.0 || params.eccentricity >= 1.0) params.eccentricity = 0.0001;
    if (params.epoch_jd <= 0.0) params.epoch_jd = julian_date_now();
    if (params.epoch_iso.empty()) params.epoch_iso = iso_utc_now();
    if (params.mean_anomaly_deg == 0.0 && params.true_anomaly != 0.0) params.mean_anomaly_deg = params.true_anomaly;
    params.semi_major_axis_km = semi_major_axis_from_mean_motion_km(params.mean_motion_rev_per_day);
    params.period_minutes = orbital_period_minutes(params.mean_motion_rev_per_day);
    if (params.tle_line1.empty() || params.tle_line2.empty()) {
        const int satnum = params.plane * 1000 + params.position_in_plane + 1;
        params.tle_line1 = make_tle_line1(satnum, params.epoch_jd, params.bstar);
        params.tle_line2 = make_tle_line2(
            satnum,
            params.inclination_deg,
            params.raan,
            params.eccentricity,
            params.argument_of_perigee_deg,
            params.mean_anomaly_deg,
            params.mean_motion_rev_per_day
        );
    }
}

PropagationResult propagate(const OrbitalParams& raw_params, double minutes_since_epoch) {
    OrbitalParams params = raw_params;
    ensure_sgp4_defaults(params, raw_params.altitude_km, raw_params.inclination_deg);

    const double a = semi_major_axis_from_mean_motion_km(params.mean_motion_rev_per_day);
    const double e = clamp(params.eccentricity, 0.0, 0.25);
    const double p = a * (1.0 - e * e);
    const double inc = deg_to_rad(params.inclination_deg);
    const double n_rad_min = params.mean_motion_rev_per_day * kTwoPi / kMinutesPerDay;
    const double coeff = 1.5 * kJ2 * (kEarthRadiusKm * kEarthRadiusKm) / (p * p) * n_rad_min;
    const double raan_rate = -coeff * std::cos(inc);
    const double argp_rate = 0.5 * coeff * (5.0 * std::cos(inc) * std::cos(inc) - 1.0);
    const double mean_rate = n_rad_min + 0.5 * coeff * std::sqrt(std::max(1e-9, 1.0 - e * e))
        * (3.0 * std::cos(inc) * std::cos(inc) - 1.0);

    const double raan = deg_to_rad(params.raan) + raan_rate * minutes_since_epoch;
    const double argp = deg_to_rad(params.argument_of_perigee_deg) + argp_rate * minutes_since_epoch;
    const double mean = deg_to_rad(params.mean_anomaly_deg) + mean_rate * minutes_since_epoch;
    const double wrapped_mean = std::fmod(mean, kTwoPi);
    const double ecc_anomaly = solve_kepler(wrapped_mean, e);
    const double cos_e = std::cos(ecc_anomaly);
    const double sin_e = std::sin(ecc_anomaly);
    const double radius = a * (1.0 - e * cos_e);
    const double true_anomaly = std::atan2(std::sqrt(std::max(0.0, 1.0 - e * e)) * sin_e, cos_e - e);

    const double u = argp + true_anomaly;
    const double cos_u = std::cos(u);
    const double sin_u = std::sin(u);
    const double cos_raan = std::cos(raan);
    const double sin_raan = std::sin(raan);
    const double cos_i = std::cos(inc);
    const double sin_i = std::sin(inc);

    PropagationResult out;
    out.coordinates.x = radius * (cos_raan * cos_u - sin_raan * sin_u * cos_i);
    out.coordinates.y = radius * (sin_raan * cos_u + cos_raan * sin_u * cos_i);
    out.coordinates.z = radius * (sin_u * sin_i);
    out.coordinates.lat = rad_to_deg(std::asin(clamp(out.coordinates.z / std::max(1.0, radius), -1.0, 1.0)));
    out.coordinates.lon = rad_to_deg(std::atan2(out.coordinates.y, out.coordinates.x));
    out.true_anomaly_deg = wrap_deg(rad_to_deg(true_anomaly));
    out.mean_anomaly_deg = wrap_deg(rad_to_deg(wrapped_mean));
    out.raan_deg = wrap_deg(rad_to_deg(raan));
    out.argument_of_perigee_deg = wrap_deg(rad_to_deg(argp));
    out.altitude_km = radius - kEarthRadiusKm;
    out.minutes_since_epoch = minutes_since_epoch;
    return out;
}

void propagate_inplace(Satellite& sat, double minutes_since_epoch) {
    ensure_sgp4_defaults(sat.orbital_params, sat.orbital_params.altitude_km, sat.orbital_params.inclination_deg);
    const double prior_minutes = sat.orbital_params.propagation_minutes;
    const double total_minutes = prior_minutes + std::max(0.0, minutes_since_epoch);
    const auto propagated = propagate(sat.orbital_params, total_minutes);
    sat.coordinates = propagated.coordinates;
    sat.orbital_params.true_anomaly = propagated.true_anomaly_deg;
    sat.orbital_params.mean_anomaly_deg = propagated.mean_anomaly_deg;
    sat.orbital_params.raan = propagated.raan_deg;
    sat.orbital_params.argument_of_perigee_deg = propagated.argument_of_perigee_deg;
    sat.orbital_params.altitude_km = propagated.altitude_km;
    sat.orbital_params.propagation_minutes = total_minutes;
    sat.orbital_params.semi_major_axis_km = semi_major_axis_from_mean_motion_km(sat.orbital_params.mean_motion_rev_per_day);
    sat.orbital_params.period_minutes = orbital_period_minutes(sat.orbital_params.mean_motion_rev_per_day);
}

} // namespace sfc::sgp4
