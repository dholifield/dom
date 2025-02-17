#include "appa.h"

namespace appa {

/* Chassis */
Chassis::Chassis(std::initializer_list<int8_t> left_motors,
                 std::initializer_list<int8_t> right_motors, Odom& odom, MoveConfig move_config,
                 TurnConfig turn_config, Options default_options)
    : left_motors(left_motors),
      right_motors(right_motors),
      odom(odom),
      move_config(move_config),
      turn_config(turn_config),
      df_options(default_options) {}

Chassis::~Chassis() { stop(true); }

void Chassis::task() {
    bool driving = false;

    const int dt = 10; // ms
    uint32_t now = pros::millis();

    // chassis loop
    while (true) {
        // wait for notify
        while (true) {
            /// TODO: wait for notify
        }

        // set variables
        chassis_mutex.take();
        const Motion motion = cmd.motion;
        const Pose target = cmd.target;
        const Options opts = cmd.options;
        chassis_mutex.give();

        uint32_t start_time = pros::millis();
        const int timeout = opts.timeout.value();

        Direction dir = opts.dir.value();
        const bool auto_dir = dir == AUTO;
        const Direction turn_dir = opts.turn.value();
        const double exit = opts.exit.value();
        const double max_speed = opts.speed.value();
        const double accel_step = opts.accel.value() * dt / 1000;
        const bool thru = opts.thru.value();

        PID lin_PID(opts.lin_PID.value());
        PID ang_PID(opts.ang_PID.value());

        Pose pose;
        Point error, carrot, speeds;
        double lin_speed, ang_speed;

        /// TODO: confirm control loop
        // control loop
        while (true) {
            // get error
            pose = odom.get();
            if (motion == MOVE_POSE || MOVE_POINT) {
                if (motion == MOVE_POSE) {
                    // calculate carrot point and set to target
                } else
                    carrot = target.p();
                error = (pose.dist(carrot), pose.angle(carrot));
            } else if (motion == TURN) {
                error = (0.0, std::fmod(target.theta - pose.theta, M_PI));
            }

            // determine direction
            if (motion == MOVE_POSE || MOVE_POINT) {
                if (auto_dir) {
                    if (fabs(error.angular) > M_PI_2)
                        dir = REVERSE;
                    else
                        dir = FORWARD;
                }
                if (dir == REVERSE) {
                    error.angular += error.angular > 0 ? -M_PI : M_PI;
                    error.linear *= -1;
                }
            } else if (motion == TURN) {
                if (turn_dir == CW && error.angular < 0)
                    error.angular += 2 * M_PI;
                else if (turn_dir == CCW && error.angular > 0)
                    error.angular -= 2 * M_PI;
            }

            // update PID
            lin_speed = lin_PID.update(error.linear, dt);
            ang_speed = ang_PID.update(error.angular, dt);

            // limit speeds
            lin_speed = limit(lin_speed, max_speed);
            ang_speed = limit(ang_speed, max_speed);

            // calculate motor speeds
            speeds = (lin_speed - ang_speed, lin_speed + ang_speed);

            // scale motor speeds
            if (speeds.left > max_speed) {
                speeds.left = max_speed;
                speeds.right *= max_speed / speeds.left;
            }
            if (speeds.right > max_speed) {
                speeds.right = max_speed;
                speeds.left *= max_speed / speeds.right;
            }

            // limit acceleration
            if (accel_step) {
                if (speeds.left - prev_speeds.left > accel_step)
                    speeds.left = prev_speeds.left + accel_step;
                if (speeds.right - prev_speeds.right > accel_step)
                    speeds.right = prev_speeds.right + accel_step;
            }

            // set motor speeds
            tank(speeds);

            // check exit conditions
            if (timeout > 0 && pros::millis() - start_time > timeout) break;
            if (error.linear < exit) break;

            // delay task
            pros::c::task_delay_until(&now, dt);
        }
    }
}

void Chassis::wait() {
    // implement
}

void Chassis::move_task(Point target, Options opts) {
    // set up variables
    Direction dir = opts.dir.value_or(df_options.dir.value_or(AUTO));
    bool auto_dir = dir == AUTO;

    double exit = opts.exit.value_or(move_config.exit);
    // int settle = opts.settle.value_or(df_options.settle.value_or(0));
    int timeout = opts.timeout.value_or(df_options.timeout.value_or(0));

    double max_speed = opts.speed.value_or(move_config.speed);
    double accel = opts.accel.value_or(df_options.accel.value_or(0));

    PID lin_PID(opts.lin_PID.value_or(move_config.lin_PID));
    PID ang_PID(opts.ang_PID.value_or(move_config.ang_PID));

    bool thru = opts.thru.value_or(df_options.thru.value_or(false));
    bool relative = opts.relative.value_or(df_options.relative.value_or(false));

    Pose pose = odom.get();
    Point error = (pose.dist(target), pose.angle(target));

    lin_PID.reset(error.linear);
    ang_PID.reset(error.angular);

    double lin_speed, ang_speed;
    Point speeds;

    // if relative motion
    if (relative) target = pose.p() + target.rotate(pose.theta);

    // timing
    int dt = 10; // ms
    int start_time = pros::millis();
    uint32_t now = pros::millis();

    double accel_step = accel * dt / 1000;

    // control loop
    while (true) {
        // calculate error
        pose = odom.get();

        error = (pose.dist(target), pose.angle(target));

        // determine direction
        if (auto_dir) {
            if (fabs(error.angular) > M_PI_2)
                dir = REVERSE;
            else
                dir = FORWARD;
        }
        if (dir == REVERSE) {
            error.angular += error.angular > 0 ? -M_PI : M_PI;
            error.linear *= -1;
        }

        // calculate PID
        lin_speed = thru ? max_speed : lin_PID.update(error.linear, dt);
        ang_speed = ang_PID.update(error.angular, dt);

        // apply limits
        lin_speed = limit(lin_speed, max_speed);
        ang_speed = limit(ang_speed, max_speed);

        // calculate motor speeds
        speeds = (lin_speed - ang_speed, lin_speed + ang_speed);

        // scale motor speeds
        if (speeds.left > max_speed) {
            speeds.left = max_speed;
            speeds.right *= max_speed / speeds.left;
        }
        if (speeds.right > max_speed) {
            speeds.right = max_speed;
            speeds.left *= max_speed / speeds.right;
        }

        // limit acceleration
        if (accel_step) {
            if (speeds.left - prev_speeds.left > accel_step)
                speeds.left = prev_speeds.left + accel_step;
            if (speeds.right - prev_speeds.right > accel_step)
                speeds.right = prev_speeds.right + accel_step;
        }

        // set motor speeds
        tank(speeds);

        // check exit conditions
        if (timeout > 0 && pros::millis() - start_time > timeout) break;
        if (error.linear < exit) break;

        // delay task
        pros::c::task_delay_until(&now, dt);
    }
    stop(false);
}

void Chassis::move(Point target, Options opts) {
    // stop task if robot is already moving
    if (chassis_task) {
        chassis_task->remove();
        delete chassis_task;
    }

    // start task
    if (opts.async.value_or(df_options.async.value_or(false))) {
        chassis_task =
            new pros::Task([this, target, opts] { move_task(target, opts); }, "chassis_task");
    } else {
        move_task(target, opts);
    }
}

void Chassis::move(double target, Options options) {
    options.relative = true;
    move({target, 0}, options);
}

void Chassis::turn_task(double target, Options opts) {
    // set up variables
    Direction dir = opts.dir.value_or(df_options.dir.value_or(AUTO));
    Direction turn_dir = opts.turn.value_or(df_options.turn.value_or(AUTO));

    double exit = opts.exit.value_or(turn_config.exit);
    // int settle = opts.settle.value_or(df_turn_opts.settle.value_or(250));
    int timeout = opts.timeout.value_or(df_options.timeout.value_or(0));

    double max_speed = opts.speed.value_or(turn_config.speed);
    double accel = opts.accel.value_or(df_options.accel.value_or(0));

    PID ang_PID(opts.ang_PID.value_or(turn_config.ang_PID));

    bool thru = opts.thru.value_or(df_options.thru.value_or(false));
    bool relative = opts.relative.value_or(df_options.relative.value_or(false));

    double heading = odom.get().theta;
    double error = heading - target;

    ang_PID.reset(error);

    double ang_speed;
    Point speeds;

    // if relative motion
    if (relative) target += heading;
    if (dir == REVERSE) target += M_PI;

    // timing
    int dt = 10; // ms
    int start_time = pros::millis();
    uint32_t now = pros::millis();

    double accel_step = accel * dt / 1000;

    // control loop
    while (true) {
        // calculate error
        heading = odom.get().theta;

        error = std::fmod(target - heading, M_PI);

        // determine direction
        if (turn_dir == CW && error < 0)
            error += 2 * M_PI;
        else if (turn_dir == CCW && error > 0)
            error -= 2 * M_PI;

        // calculate PID
        ang_speed = thru ? (error > 0 ? max_speed : -max_speed) : ang_PID.update(error, dt);

        // apply limits
        ang_speed = limit(ang_speed, max_speed);

        // calculate motor speeds
        speeds = (-ang_speed, ang_speed);

        // limit acceleration
        if (accel_step) {
            if (speeds.left - prev_speeds.left > accel_step)
                speeds.left = prev_speeds.left + accel_step;
            if (speeds.right - prev_speeds.right > accel_step)
                speeds.right = prev_speeds.right + accel_step;
        }

        // set motor speeds
        tank(speeds);

        // check exit conditions
        if (timeout > 0 && pros::millis() - start_time > timeout) break;
        if (error < exit) break;

        // delay task
        pros::c::task_delay_until(&now, dt);
    }
    stop(false);
}

void Chassis::turn(double target, Options opts) {
    // stop task if robot is already moving
    if (chassis_task) {
        chassis_task->remove();
        delete chassis_task;
    }

    // convert to radians
    target = to_rad(target);

    // start task
    if (opts.async.value_or(df_options.async.value_or(false))) {
        chassis_task =
            new pros::Task([this, target, opts] { turn_task(target, opts); }, "chassis_task");
    } else {
        turn_task(target, opts);
    }
}

void Chassis::turn(Point target, Options options) {
    double heading = to_deg(odom.get().p().angle(target));

    turn(heading, options);
}

void Chassis::tank(double left_speed, double right_speed) {
    left_motors.move_voltage(left_speed * 120);
    right_motors.move_voltage(right_speed * 120);
    std::lock_guard<pros::Mutex> lock(chassis_mutex);
    prev_speeds = (left_speed, right_speed);
}

void Chassis::tank(Point speeds) { tank(speeds.left, speeds.right); }

void Chassis::tank(pros::Controller& controller) {
    double left_speed = controller.get_analog(pros::E_CONTROLLER_ANALOG_LEFT_Y) / 1.27;
    double right_speed = controller.get_analog(pros::E_CONTROLLER_ANALOG_RIGHT_Y) / 1.27;
    tank(left_speed, right_speed);
}

void Chassis::arcade(double linear, double angular) {
    double left_speed = linear + angular;
    double right_speed = linear - angular;
    tank(left_speed, right_speed);
}

void Chassis::arcade(pros::Controller& controller) {
    double linear = controller.get_analog(pros::E_CONTROLLER_ANALOG_LEFT_Y) / 1.27;
    double angular = controller.get_analog(pros::E_CONTROLLER_ANALOG_RIGHT_X) / 1.27;
    arcade(linear, angular);
}

void Chassis::stop(bool stop_task) {
    if (stop_task && chassis_task) {
        chassis_task->remove();
        delete chassis_task;
        chassis_task = nullptr;
    }
    tank(0, 0);
}

void Chassis::set_brake_mode(pros::motor_brake_mode_e_t mode) {
    left_motors.set_brake_mode_all(mode);
    right_motors.set_brake_mode_all(mode);
}

} // namespace appa

/**
 * TODO: add settle time
 * TODO: implement settle exit for stuck robot
 * TODO: make all movements run in 1 task
 */