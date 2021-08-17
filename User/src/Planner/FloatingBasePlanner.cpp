//
// Created by nimpng on 7/25/21.
//

#include "Planner/FloatingBasePlanner.h"

FloatingBasePlanner::FloatingBasePlanner(Poplar::Index horizons, Scalar mpc_dt, Scalar dt) :
        base("torso"), srgbMpc(horizons, mpc_dt, 8) {
    _dt = dt;
    _mpc_dt = mpc_dt;
    contactTable = MatInt(8, horizons);

    xDes = 0;
    yDes = 0;
    maxPosError = 0.1;
    bodyHeight = 0.892442; // TODO
    yawDes = 0.;
    rollDes = 0.;
    pitchDes = 0.;
    yawdDes = 0;

    rpInt.setZero();
    rpCom.setZero();
    trajInit.setZero();

    x0.setZero();

    X_d.resize(13 * horizons, Eigen::NoChange);
    X_d.setZero();
    contactTable.setZero();

    q_soln.resize(12 * horizons);
    q_soln.setZero();

    Vec Qx(12);
    Qx.segment(0, 3) << 100.5, 100.5, 10.0;
    Qx.segment(3, 3) << 10, 10, 50;
    Qx.segment(6, 3) << 0.1, 0.1, 0.3;
    Qx.segment(9, 3) << 10.1, 10.1, 0.3;
    Vec Qf(3);
    Qf.fill(4e-5);
    srgbMpc.setWeight(Qx, Qf);
    srgbMpc.setMassAndInertia(mass, Ibody);
    srgbMpc.setMaxForce(500);

    LIPM_Parameters param;
    param.Qx << 10, 1, 10, 1;
    param.Qu << 1e-5, 1e-5;
    param.mpc_dt = 0.05;
    param.mpc_horizons = 20;
    param.height = bodyHeight;
    lipmMpc.setParamters(param);
}

void FloatingBasePlanner::plan(size_t iter, const RobotState &state, RobotWrapper &robot, const GaitData &gaitData,
                               Tasks &tasks) {
    mass = robot.totalMass();
    Ibody = robot.Ig();
    srgbMpc.setMassAndInertia(mass, Ibody);

    auto base_pose = robot.frame_pose(base);
    auto base_vel = robot.frame_6dVel_localWorldAligned(base);
    Vec3 rpy = pin::rpy::matrixToRpy(base_pose.rotation());
    Vec3 c = robot.CoM_pos();
    Vec3 cdot = robot.CoM_vel();
    /*Vec3 c = base_pose.translation();
    Vec3 cdot = robot.frame_6dVel_localWorldAligned(base).linear();*/

    // TODO:
    Vec3 vBodyDes(0, 0, 0);
    Scalar yawd_des = 0;
    Vec3 vWorldDes = robot.frame_pose(base).rotation() * vBodyDes;
    if (iter == 0) {
        xDes = c[0];
        yDes = c[1];
        yawDes = rpy[2];
    } else {
        xDes += vWorldDes[0] * _dt;
        yDes += vWorldDes[1] * _dt;
        yawDes = rpy[2] + yawd_des * _dt;
    }
    rpInt[0] = _dt * (rollDes - rpy[0]);
    rpInt[1] = _dt * (rollDes - rpy[1]);
    rpCom[0] = rollDes + rpInt[0];
    rpCom[1] = pitchDes + 0.5 * (rollDes - rpy[0]) + rpInt[1];

    if (iter % 15 == 0) {
        x0 << rpy, c, base_vel.angular(), cdot, -9.81;
        srgbMpc.setCurrentState(x0);
        generateRefTraj(state, robot);
        srgbMpc.setDesiredDiscreteTrajectory(X_d);
        generateContactTable(gaitData);
        srgbMpc.setContactTable(contactTable);

        Vec3 r;
        vector<Vec3> cp;
        auto cl = robot.contactVirtualLinks();
        for (int i(0); i < cl.size(); i++) {
            r = robot.frame_pose(cl[i]).translation();
            /*std::cout << "r " << i << ": " << r.transpose() << "\n";*/
            cp.push_back(r);
        }
        srgbMpc.setContactPointPos(cp);
        Timer timer;
        srgbMpc.solve(0.0);
        printf("mpc solve time: %f\n", timer.getMs());
        /*std::cout << "contact force: " << srgbMpc.getCurrentDesiredContactForce().transpose() << std::endl;
        std::cout << "optimal traj: " << srgbMpc.getDiscreteOptimizedTrajectory().transpose() << std::endl;*/
    }
    auto xopt = srgbMpc.getDiscreteOptimizedTrajectory();
    auto xdd = srgbMpc.getXDot();
    tasks.floatingBaseTask.pos = xopt.segment(3, 3);
    tasks.forceTask = srgbMpc.getCurrentDesiredContactForce();
    tasks.floatingBaseTask.vel = xopt.segment(9, 3);
    tasks.floatingBaseTask.acc = xdd.segment(9, 3);
    tasks.floatingBaseTask.omega_dot = xdd.segment(6, 3);
    tasks.floatingBaseTask.R_wb = pin::rpy::rpyToMatrix(xopt.segment(0, 3));
    tasks.floatingBaseTask.omega = xopt.segment(6, 3);

    lipmMpc.x0() << c(0), cdot(0), c(1), cdot(1);
    auto &param = lipmMpc.parameters();
    Mat C = Mat::Zero(param.mpc_horizons * 2, 2 * param.mpc_horizons);
    Vec c_lb = Vec::Zero(param.mpc_horizons * 2);
    Vec c_ub = Vec::Zero(param.mpc_horizons * 2);
    for (int i = 0; i < param.mpc_horizons; i++) {
        C.block<2, 2>(i * 2, i * 2).setIdentity();
        c_lb.segment(i * 2, 2) << -0.5, -0.5;
        c_ub.segment(i * 2, 2) << 0.5, 0.5;
    }
    lipmMpc.updateZMP_constraints(C, c_lb, c_ub);

    lipmMpc.run();
}

void FloatingBasePlanner::generateRefTraj(const RobotState &state, RobotWrapper &robot) {
    Vec3 vBodyDes(0, 0, 0); // TODO:
    Vec3 vWorldDes = robot.frame_pose(base).rotation() * vBodyDes;
    Vec3 pos = robot.frame_pose(base).translation();

    if (xDes - pos[0] > maxPosError)
        xDes = pos[0] + maxPosError;
    if (pos[0] - xDes > maxPosError)
        xDes = pos[0] - maxPosError;

    if (yDes - pos[1] > maxPosError)
        yDes = pos[1] + maxPosError;
    if (pos[1] - yDes > maxPosError)
        yDes = pos[1] - maxPosError;

    trajInit << rpCom[0], rpCom[1], yawDes,
            xDes, yDes, bodyHeight,
            0.0, 0.0, yawdDes,
            vWorldDes;
    X_d.head(12) = trajInit;
    for (int i = 1; i < srgbMpc.horizons(); i++) {
        for (int j = 0; j < 12; j++)
            X_d[13 * i + j] = trajInit[j];

        X_d[13 * i + 2] = trajInit[2] + i * yawdDes * _mpc_dt;
        X_d.segment(13 * i + 3, 3) = trajInit.segment(3, 3) +
                                     i * vWorldDes * _mpc_dt;
        //        std::cout << "x_d " << i << ": " << X_d.segment(13 * i, 13).transpose() << std::endl;
    }
}

void FloatingBasePlanner::generateContactTable(const GaitData &gaitData) {
    contactTable.setZero();
    for (int i = 0; i < srgbMpc.horizons(); i++) {
        if (gaitData.contactTable(0, i) == 1) {
            contactTable.col(i).head(4).fill(1);
        }
        if (gaitData.contactTable(1, i) == 1) {
            contactTable.col(i).tail(4).fill(1);
        }
    }
}

ConstVecRef FloatingBasePlanner::getOptimalTraj() {
//    return srgbMpc.getDiscreteOptimizedTrajectory();
    return lipmMpc.optimalTraj();
}

ConstVecRef FloatingBasePlanner::getDesiredTraj() {
    return ConstVecRef(X_d);
}
