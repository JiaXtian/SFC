#pragma once

#include "models/types.h"
#include <string>

namespace sfc::sgp4 {

struct PropagationResult {
    Coordinates coordinates;
    double true_anomaly_deg = 0.0;
    double mean_anomaly_deg = 0.0;
    double raan_deg = 0.0;
    double argument_of_perigee_deg = 0.0;
    double altitude_km = 0.0;
    double minutes_since_epoch = 0.0;
};

double julian_date_from_unix(double unix_seconds);
double julian_date_now();
std::string iso_utc_now();

double mean_motion_from_altitude_rev_per_day(double altitude_km);
double semi_major_axis_from_mean_motion_km(double mean_motion_rev_per_day);
double orbital_period_minutes(double mean_motion_rev_per_day);
bool parse_tle_into_params(OrbitalParams& params, const std::string& tle_line1, const std::string& tle_line2, std::string* error = nullptr);

OrbitalParams make_walker_sgp4_params(
    int plane,
    int position_in_plane,
    int total_planes,
    int sats_per_plane,
    double altitude_km,
    double inclination_deg,
    int phasing = 0,
    double epoch_jd = 0.0
);

void ensure_sgp4_defaults(OrbitalParams& params, double fallback_altitude_km, double fallback_inclination_deg);
PropagationResult propagate(const OrbitalParams& params, double minutes_since_epoch);
void propagate_inplace(Satellite& sat, double minutes_since_epoch);

} // namespace sfc::sgp4
