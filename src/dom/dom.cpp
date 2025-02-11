#include "dom.h"

namespace dom {

/* Odom */
Odom::Odom(int x_port, int y_port, int imu_port, int tpi, Point tracker_linear_offset, double tracker_angular_offset) :
        tpi(tpi),
        tracker_linear_offset(tracker_linear_offset),
        tracker_angular_offset(to_rad(tracker_angular_offset)),
        imu(imu_port),
        x_tracker(abs(x_port), abs(x_port) + 1, x_port < 0),
        y_tracker(abs(y_port), abs(y_port) + 1, y_port < 0) {}

Odom::Odom(std::array<int8_t, 2> x_port, std::array<int8_t, 2> y_port, int imu_port, int tpi, Point tracker_linear_offset, double tracker_angular_offset) :
        tpi(tpi),
        tracker_linear_offset(tracker_linear_offset),
        tracker_angular_offset(to_rad(tracker_angular_offset)),
        imu(imu_port),
        x_tracker({x_port[0], abs(x_port[1]), abs(x_port[1]) + 1}, x_port[1] < 0),
        y_tracker({y_port[0], abs(y_port[1]), abs(y_port[1]) + 1}, y_port[1] < 0) {}

void Odom::task() {
    printf("odom task started\n");
    Pose prev_track = {0, 0, 0};
    uint32_t now = pros::millis();

    while(true) {
        // get current sensor values
        Pose track = {x_tracker.get_value() / tpi,
                      y_tracker.get_value() / tpi,
                      to_rad(-imu.get_rotation())};

        // calculate change in sensor values
        Point dtrack = {track.x - prev_track.x,
                        track.y - prev_track.y};
        double dtheta = track.theta - prev_track.theta;

        // set previous sensor values for next loop
        prev_track = track;
        
        // arc approximation
        if (dtheta != 0)
            dtrack *= 2 * sin(dtheta / 2) / dtheta;

        // rotate tracker differential to global frame
        dtrack = dtrack.rotate(track.theta + tracker_angular_offset);

        // update tracker pose
        odom_mutex.take();
        odom_pose += dtrack;
        odom_pose.theta = track.theta;
        odom_mutex.give();
        
        // loop every 5 ms
        pros::c::task_delay_until(&now, 5);
    }
}

void Odom::start() {
    printf("calibrating imu... ");
    imu.reset(true);
    imu.set_data_rate(5);
    printf("done\n");

    set({0, 0, 0});
    if (odom_task == nullptr)
        odom_task = new pros::Task([this] { task(); }, 16, TASK_STACK_DEPTH_DEFAULT, "odom_task");
}

Pose Odom::get() {
    std::lock_guard<pros::Mutex> lock(odom_mutex);
    // translate the tracker offsets to the global frame
    return odom_pose + tracker_linear_offset.rotate(odom_pose.theta);
}

Pose Odom::get_local() {
    std::lock_guard<pros::Mutex> lock(odom_mutex);
    return odom_pose;
}

void Odom::set(Pose pose) {
    const std::lock_guard<pros::Mutex> lock(odom_mutex);
    imu.set_rotation(-pose.theta);
    odom_pose = pose;
}

void Odom::set_point(Point point) {
    std::lock_guard<pros::Mutex> lock(odom_mutex);
    odom_pose.x = point.x;
    odom_pose.y = point.y;
}

void Odom::set_x(double x) {
    std::lock_guard<pros::Mutex> lock(odom_mutex);
    odom_pose.x = x;
}

void Odom::set_y(double y) {
    std::lock_guard<pros::Mutex> lock(odom_mutex);
    odom_pose.y = y;
}

void Odom::set_theta(double theta) {
    std::lock_guard<pros::Mutex> lock(odom_mutex);
    imu.set_rotation(-theta);
    odom_pose.theta = theta;
}

void Odom::set(Point point, double theta) { set({point.x, point.y, theta}); }
void Odom::set(double x, double y, double theta) { set({x, y, theta}); }
void Odom::set_point(double x, double y) { set_point({x, y}); }

void Odom::set_offset(Point linear) {
    std::lock_guard<pros::Mutex> lock(odom_mutex);
    tracker_linear_offset = linear;
}

void Odom::debug() {
    printf("odom_task priority: %d\n", odom_task->get_priority());
    Pose pose = get();
    printf("x: %.2f, y: %.2f, theta: %.2f\n", pose.x, pose.y, to_deg(pose.theta));
}

/* Chassis */
Chassis::Chassis(std::initializer_list<int8_t> left_motors,
                 std::initializer_list<int8_t> right_motors,
                 Odom& odom,
                 Options default_move_options,
                 Options default_turn_options)
    : left_motors(left_motors),
      right_motors(right_motors),
      odom(odom),
      df_move_opts(default_move_options),
      df_turn_opts(default_turn_options) {}
      
Chassis::~Chassis() {
    stop();
}

void Chassis::init() {
    set_brake_mode(pros::E_MOTOR_BRAKE_COAST);
}

void Chassis::wait() {
    if (chassis_task)
        chassis_task->join();
}

void Chassis::move_task(Point target, Options opts) {
    // set up variables
    Direction dir = opts.dir.value_or(df_move_opts.dir.value_or(AUTO));
    bool auto_dir = dir == AUTO;

    double exit = opts.exit.value_or(df_move_opts.exit.value_or(1.0));
    // int settle = opts.settle.value_or(df_move_opts.settle.value_or(250));
    int timeout = opts.timeout.value_or(df_move_opts.timeout.value_or(10000));

    double speed = opts.speed.value_or(df_move_opts.speed.value_or(100));
    // double accel = opts.accel.value_or(df_move_opts.accel.value_or(50));

    PID lin_PID(opts.lin_PID.value_or(df_move_opts.lin_PID.value_or(Gains{10, 0, 0})));
    PID ang_PID(opts.ang_PID.value_or(df_move_opts.ang_PID.value_or(Gains{100, 0, 0})));

    bool thru = opts.thru.value_or(df_move_opts.thru.value_or(false));
    bool relative = opts.relative.value_or(df_move_opts.relative.value_or(false));

    Pose pose = odom.get();
    double lin_error = pose.dist(target);
    double ang_error = pose.angle(target);

    lin_PID.reset(lin_error);
    ang_PID.reset(ang_error);

    double lin_speed, ang_speed;
    double left_speed, right_speed;
    
    // if relative motion
    if (relative) target = pose.p() + target.rotate(pose.theta);

    // timing
    int dt = 10; // ms
    int start_time = pros::millis();
    uint32_t now = pros::millis();

    // control loop
    while(lin_error > exit && (timeout != 0 && pros::millis() - start_time < timeout)) {
        // calculate error
        pose = odom.get();
        lin_error = pose.dist(target);
        ang_error = pose.angle(target);

        // determine direction
        if (auto_dir) {
            if (fabs(ang_error) > M_PI_2) dir = REVERSE;
            else dir = FORWARD;
        }
        if (dir == REVERSE) ang_error = M_PI - ang_error;

        // calculate PID
        if (thru) lin_speed = speed;
        else lin_speed = lin_PID.update(lin_error, dt);
        ang_speed = ang_PID.update(ang_error, dt);

        // apply limits
        lin_speed = limit(lin_speed, speed);
        ang_speed = limit(ang_speed, speed);

        // calculate motor speeds
        left_speed = lin_speed - ang_speed;
        right_speed = lin_speed + ang_speed;

        // scale motor speeds
        if (left_speed > speed) { 
            left_speed = speed;
            right_speed *= speed / left_speed;
        } else if (right_speed > speed) {
            right_speed = speed;
            left_speed *= speed / right_speed;
        }

        // limit acceleration


        // set motor speeds
        tank(left_speed, right_speed);

        // delay task
        pros::c::task_delay_until(&now, dt);
    }
}

void Chassis::move(Point target, Options opts) {
    // stop task if robot is already moving
    if (chassis_task)
        chassis_task->remove();
        delete chassis_task;

    // start task
    chassis_task = new pros::Task([this, target, opts] { move_task(target, opts); }, "chassis_task");

    // wait if not asynchroneous
    if (!opts.async.value_or(df_move_opts.async.value_or(false))) {
        wait();
    }
}

void Chassis::move(double target, Options options) {
    options.relative = true;
    move({target, 0}, options);
}

void Chassis::turn(Point target, Options options) {

}

void Chassis::turn(double target, Options options) {

}

void Chassis::tank(double left_speed, double right_speed) {
    left_motors.move_voltage(left_speed * 120);
    right_motors.move_voltage(right_speed * 120);
}

void Chassis::arcade(double linear, double angular) {
    double left_speed = linear + angular;
    double right_speed = linear - angular;
    tank(left_speed, right_speed);
}

void Chassis::arcade(pros::Controller& controller) {
    double linear = controller.get_analog(pros::E_CONTROLLER_ANALOG_LEFT_Y);
    double angular = controller.get_analog(pros::E_CONTROLLER_ANALOG_RIGHT_X);
    arcade(linear, angular);
}

void Chassis::stop() {
    if (chassis_task) {
        chassis_task->remove();
        delete chassis_task;
        chassis_task = nullptr;
    }
    tank(0, 0);
}

void Chassis::set_brake_mode(pros::motor_brake_mode_e_t mode) {
    left_motors.set_brake_mode(mode);
    right_motors.set_brake_mode(mode);
}

/* */
} // namespace dom