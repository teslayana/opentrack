/* Copyright (c) 2012-2013 Stanislaw Halik <sthalik@misaki.pl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

/*
 * this file appeared originally in facetracknoir, was rewritten completely
 * following opentrack fork.
 *
 * originally written by Wim Vriend.
 */


#include "tracker.h"
#include <cmath>
#include <algorithm>

#if defined(_WIN32)
#   include <windows.h>
#endif

Tracker::Tracker(main_settings& s, Mappings &m, SelectedLibraries &libs) :
    s(s),
    m(m),
    centerp(false),
    enabledp(true),
    should_quit(false),
    libs(libs),
    t_b {0,0,0}
{
}

Tracker::~Tracker()
{
    should_quit = true;
    wait();
}

double Tracker::map(double pos, bool invertp, Mapping& axis)
{
    bool altp = (pos < 0) == !invertp && axis.opts.altp;
    axis.curve.setTrackingActive( !altp );
    axis.curveAlt.setTrackingActive( altp );
    auto& fc = altp ? axis.curveAlt : axis.curve;
    return fc.getValue(pos) + axis.opts.zero;
}

// tait-bryan angles, not euler
static dmat<3, 3> euler_to_rmat(const double* input)
{
    static constexpr double pi = 3.141592653;
    const auto H = input[1] * pi / 180;
    const auto P = input[0] * pi / 180;
    const auto B = input[2] * pi / 180;

    const auto c1 = cos(H);
    const auto s1 = sin(H);
    const auto c2 = cos(P);
    const auto s2 = sin(P);
    const auto c3 = cos(B);
    const auto s3 = sin(B);

    double foo[] = {
        //Tait-Bryan XYZ
        c2*c3,  -c2*s3,  s2,
        c1*s3+c3*s1*s2,  c1*c3-s1*s2*s3,  -c2*s1,
        s1*s3-c1*c3*s2,  c3*s1+c1*s2*s3,  c1*c2,
    };

    return dmat<3, 3>(foo);
}

void Tracker::t_compensate(const dmat<3, 3>& rmat, const double* xyz, double* output, bool rz)
{
    static constexpr int p_x = 0, p_y = 1, p_z = 2;
    const double xyz_[3] = { -xyz[p_x], -xyz[p_y], xyz[p_z] };
    dmat<3, 1> tvec(xyz_);
    const dmat<3, 1> ret = rmat * tvec;
    output[0] = -ret(p_x, 0);
    output[1] = -ret(p_y, 0);
    if (!rz)
        output[2] = ret(p_z, 0);
    else
        output[2] = xyz[2];
}

void Tracker::logic()
{
    if (enabledp)
        for (int i = 0; i < 6; i++)
            final_raw(i) = newpose[i];

    Pose filtered_pose;

    if (libs.pFilter)
        libs.pFilter->filter(final_raw, filtered_pose);
    else
        filtered_pose = final_raw;

    bool inverts[6] = {
        m(0).opts.invert,
        m(1).opts.invert,
        m(2).opts.invert,
        m(3).opts.invert,
        m(4).opts.invert,
        m(5).opts.invert,
    };

    // must invert early as euler_to_rmat's sensitive to sign change
    for (int i = 0; i < 6; i++)
        filtered_pose[i] *= inverts[i] ? -1. : 1.;

    static constexpr double pi = 3.141592653;
    static constexpr double d2r = pi / 180.;

    Pose mapped_pose;

    for (int i = 0; i < 6; i++)
        mapped_pose(i) = map(filtered_pose(i), inverts[i], m(i));

    if (centerp)
    {
        centerp = false;
        for (int i = 0; i < 3; i++)
            t_b[i] = filtered_pose(i);
        q_b = Quat::from_euler_rads(mapped_pose(Yaw) * d2r,
                                    mapped_pose(Pitch) * d2r,
                                    mapped_pose(Roll) * d2r);
    }

    Pose centered;

    {
        const Quat q(mapped_pose(Yaw)*d2r,
                     mapped_pose(Pitch)*d2r,
                     mapped_pose(Roll)*d2r);
        const Quat q_ = q * q_b.inv();
        double ypr[3];
        q_.to_euler_degrees(ypr[0], ypr[1], ypr[2]);
        for (int i = 0; i < 3; i++)
        {
            centered(i) = mapped_pose(i) - t_b[i];
            centered(i+3) = ypr[i];
        }
    }

    Pose centered_ = centered;

    if (s.tcomp_p)
        t_compensate(euler_to_rmat(&centered[Yaw]),
                     centered,
                     centered_,
                     s.tcomp_tz);

    Pose mapped;

    for (int i = 0; i < 6; i++)
    {
        auto& axis = m(i);
        int k = axis.opts.src;
        if (k < 0 || k >= 6)
            mapped(i) = 0;
        else
            mapped(i) = centered_(i);
    }

    libs.pProtocol->pose(mapped);

    QMutexLocker foo(&mtx);
    output_pose = mapped;
    raw_6dof = final_raw;
}

void Tracker::run() {
    const int sleep_ms = 3;

#if defined(_WIN32)
    (void) timeBeginPeriod(1);
#endif

    while (!should_quit)
    {
        t.start();

        libs.pTracker->data(newpose);
        logic();

        long q = sleep_ms * 1000L - t.elapsed()/1000L;
        usleep(std::max(1L, q));
    }

    {
        // do one last pass with origin pose
        for (int i = 0; i < 6; i++)
            newpose[i] = 0;
        logic();
        // filter may inhibit exact origin
        Pose p;
        libs.pProtocol->pose(p);
    }

#if defined(_WIN32)
    (void) timeEndPeriod(1);
#endif

    for (int i = 0; i < 6; i++)
    {
        m(i).curve.setTrackingActive(false);
        m(i).curveAlt.setTrackingActive(false);
    }
}

void Tracker::get_raw_and_mapped_poses(double* mapped, double* raw) const {
    QMutexLocker foo(&const_cast<Tracker&>(*this).mtx);
    for (int i = 0; i < 6; i++)
    {
        raw[i] = raw_6dof(i);
        mapped[i] = output_pose(i);
    }
}

