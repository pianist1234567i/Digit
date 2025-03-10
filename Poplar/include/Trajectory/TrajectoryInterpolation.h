//
// Created by nimapng on 7/17/21.
//

#ifndef TRAJECTORY_TRAJECTORYINTERPOLATION_H
#define TRAJECTORY_TRAJECTORYINTERPOLATION_H

#include "Trajectory/Trajectory.h"

#include  "Trajectory/Spline.h"

using namespace tk;

class TrajectoryInterpolation : public Trajectory {
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    TrajectoryInterpolation(size_t dims = 1, Spline::spline_type line_type = Spline::spline_type::cspline_hermite);

    virtual SamplePoint operator()(Scalar t);

    virtual SamplePoint operator()(Scalar t, size_t dim_num);

    virtual bool setSamplePoints(vector<SamplePoint> &samplePoints);

private:
    vector<vector<Spline>> n_lines;
    vector<Scalar> end_t;
    Spline::spline_type _line_type;
};


#endif //TRAJECTORY_TRAJECTORYINTERPOLATION_H
