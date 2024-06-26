#include <rclcpp/rclcpp.hpp>

#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include "ros_g29_force_feedback/msg/force_feedback.hpp"
#include "ros_g29_force_feedback/msg/force_control.hpp"
// #include "ros_g29_force_feedback/msg/force_feedback.hpp"

class G29ForceFeedback : public rclcpp::Node {

private:
    rclcpp::Subscription<ros_g29_force_feedback::msg::ForceFeedback>::SharedPtr sub_target;
    rclcpp::Publisher<ros_g29_force_feedback::msg::ForceControl>::SharedPtr force_publisher;
    rclcpp::TimerBase::SharedPtr timer;
    // device info
    int m_device_handle;
    int m_axis_code = ABS_X;
    int m_axis_min;
    int m_axis_max;

    // rosparam
    std::string m_device_name;
    double m_loop_rate;
    double m_max_torque;
    double m_min_torque;
    double m_brake_position;
    double m_brake_torque;
    double m_auto_centering_max_torque;
    double m_auto_centering_max_position;
    double m_eps;
    bool m_auto_centering;

    // variables
    ros_g29_force_feedback::msg::ForceFeedback m_target;
    bool m_is_target_updated = false;
    bool m_is_brake_range = false;
    struct ff_effect m_effect;
    double m_position;
    double m_torque;
    double m_attack_length;
    double m_err = 0.0;
    double m_d_err = 0.0;
    double m_i_err = 0.0;
    double m_threshold = 0.0005;
    float m_prev_target = 0.0;

public:
    G29ForceFeedback();
    ~G29ForceFeedback();

private:
    void targetCallback(const ros_g29_force_feedback::msg::ForceFeedback::SharedPtr in_target);
    void loop();
    int testBit(int bit, unsigned char *array);
    void initDevice();
    void calcRotateForce(double &torque, double &attack_length, const ros_g29_force_feedback::msg::ForceFeedback &target, const double &current_position);
    void calcCenteringForce(double &torque, const ros_g29_force_feedback::msg::ForceFeedback &target, const double &current_position);
    void uploadForce(const double &force, const double &attack_length);

    ros_g29_force_feedback::msg::ForceControl buildMessage(bool is_centering, bool human_ctrl, double torque);
};


G29ForceFeedback::G29ForceFeedback() 
    : Node("g29_force_feedback"){
        
    sub_target = this->create_subscription<ros_g29_force_feedback::msg::ForceFeedback>(
        "/ff_target", 
        rclcpp::SystemDefaultsQoS(), 
        std::bind(&G29ForceFeedback::targetCallback, this, std::placeholders::_1));

    force_publisher = this->create_publisher<ros_g29_force_feedback::msg::ForceControl>(
        "/force_control", 
        rclcpp::SystemDefaultsQoS());

    declare_parameter("device_name", m_device_name);
    declare_parameter("loop_rate", m_loop_rate);
    declare_parameter("max_torque", m_max_torque);
    declare_parameter("min_torque", m_min_torque);
    declare_parameter("brake_position", m_brake_position);
    declare_parameter("brake_torque", m_brake_torque);
    declare_parameter("auto_centering_max_torque", m_auto_centering_max_torque);
    declare_parameter("auto_centering_max_position", m_auto_centering_max_position);
    declare_parameter("eps", m_eps);
    declare_parameter("auto_centering", m_auto_centering);
    declare_parameter("threshold", m_threshold);

    get_parameter("device_name", m_device_name);
    get_parameter("loop_rate", m_loop_rate);
    get_parameter("max_torque", m_max_torque);
    get_parameter("min_torque", m_min_torque);
    get_parameter("brake_position", m_brake_position);
    get_parameter("brake_torque", m_brake_torque);
    get_parameter("auto_centering_max_torque", m_auto_centering_max_torque);
    get_parameter("auto_centering_max_position", m_auto_centering_max_position);
    get_parameter("eps", m_eps);
    get_parameter("auto_centering", m_auto_centering);
    get_parameter("threshold", m_threshold);

    initDevice();

    rclcpp::sleep_for(std::chrono::seconds(1));
    // TODO-KM: Reduce rate
    timer = this->create_wall_timer(std::chrono::milliseconds((int)m_loop_rate*10), 
            std::bind(&G29ForceFeedback::loop,this));
}

G29ForceFeedback::~G29ForceFeedback() {

    m_effect.type = FF_CONSTANT;
    m_effect.id = -1;
    m_effect.u.constant.level = 0;
    m_effect.direction = 0;
    // upload m_effect
    if (ioctl(m_device_handle, EVIOCSFF, &m_effect) < 0) {
        std::cout << "failed to upload m_effect" << std::endl;
    }
}


// update input event with timer callback
void G29ForceFeedback::loop() {
    struct input_event event;
    double prev_torque = fabs(m_torque);
    double prev_position = fabs(m_position);

    // get current state
    while (read(m_device_handle, &event, sizeof(event)) == sizeof(event)) {
        if (event.type == EV_ABS && event.code == m_axis_code) {
            m_position = (event.value - (m_axis_max + m_axis_min) * 0.5) * 2 / (m_axis_max - m_axis_min);
        }
    }

    if (m_auto_centering) {
        calcCenteringForce(m_torque, m_target, m_position);
        m_attack_length = 0.0;

    } else {
        calcRotateForce(m_torque, m_attack_length, m_target, m_position);
        m_is_target_updated = false;

        double abs_position = fabs(m_position);
        double torque_compensation = fabs(prev_torque) / 1000;
        double combined = fabs(torque_compensation + prev_position);

        bool human_ctrl = fabs(abs_position - combined) > m_threshold;

        force_publisher->publish(buildMessage(false, human_ctrl, m_torque));
        
    }

    uploadForce(m_torque, m_attack_length);
}


void G29ForceFeedback::calcRotateForce(double &torque,
                                       double &attack_length,
                                       const ros_g29_force_feedback::msg::ForceFeedback &target,
                                       const double &current_position) {

    double k_p = 4.5;
    double k_d = 2.0;
    double k_i = 0.0;

    double prev_err = m_err;
    m_err = target.position - current_position;
    m_d_err = m_err - prev_err;
    m_i_err += m_err;

    torque = k_p * m_err + k_d * m_d_err + k_i * m_i_err;
    torque = std::min(m_max_torque, std::max(-m_max_torque, torque));
    attack_length = m_loop_rate;
    m_prev_target = target.position;
}


void G29ForceFeedback::calcCenteringForce(double &torque,
                                          const ros_g29_force_feedback::msg::ForceFeedback &target,
                                          const double &current_position) {

    double diff = target.position - current_position;
    double direction = (diff > 0.0) ? 1.0 : -1.0;

    if (fabs(diff) < m_eps)
        torque = 0.0;

    else {
        double torque_range = m_auto_centering_max_torque - m_min_torque;
        double power = (fabs(diff) - m_eps) / (m_auto_centering_max_position - m_eps);
        double buf_torque = power * torque_range + m_min_torque;
        torque = std::min(buf_torque, m_auto_centering_max_torque) * direction;
    }

    force_publisher->publish(buildMessage(true, false, torque));
}


// update input event with writing information to the event file
void G29ForceFeedback::uploadForce(const double &torque,
                                   const double &attack_length) {

    // std::cout << torque << std::endl;
    // set effect
    m_effect.u.constant.level = 0x7fff * std::min(torque, m_max_torque);
    m_effect.direction = 0xC000;
    m_effect.u.constant.envelope.attack_level = 0; /* 0x7fff * force / 2 */
    m_effect.u.constant.envelope.attack_length = attack_length;
    m_effect.u.constant.envelope.fade_level = 0;
    m_effect.u.constant.envelope.fade_length = attack_length;

    // upload effect
    if (ioctl(m_device_handle, EVIOCSFF, &m_effect) < 0) {
        std::cout << "failed to upload effect" << std::endl;
    }
}


// get target information of wheel control from ros message
void G29ForceFeedback::targetCallback(const ros_g29_force_feedback::msg::ForceFeedback::SharedPtr in_msg) {

    if (m_target.position == in_msg->position && m_target.torque == fabs(in_msg->torque)) {
        m_is_target_updated = false;

    } else {
        m_target = *in_msg;

        if (m_target.torque == 0) {
            m_auto_centering = true;
            return;
        }

        m_target.torque = fabs(m_target.torque);
        m_is_target_updated = true;
        m_is_brake_range = false;
        m_auto_centering = false;
    }
}


// initialize force feedback device
void G29ForceFeedback::initDevice() {
    // setup device
    unsigned char abs_bits[1+ABS_MAX/8/sizeof(unsigned char)];
    unsigned char ff_bits[1+FF_MAX/8/sizeof(unsigned char)];
    struct input_event event;
    struct input_absinfo abs_info;

    m_device_handle = open(m_device_name.c_str(), O_RDWR|O_NONBLOCK);
    if (m_device_handle < 0) {
        std::cout << "ERROR: cannot open device : "<< m_device_name << std::endl;
        exit(1);

    } else {std::cout << "device opened" << std::endl;}

    // which axes has the device?
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(m_device_handle, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
        std::cout << "ERROR: cannot get abs bits" << std::endl;
        exit(1);
    }

    // get some information about force feedback
    memset(ff_bits, 0, sizeof(ff_bits));
    if (ioctl(m_device_handle, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits) < 0) {
        std::cout << "ERROR: cannot get ff bits" << std::endl;
        exit(1);
    }

    // get axis value range
    if (ioctl(m_device_handle, EVIOCGABS(m_axis_code), &abs_info) < 0) {
        std::cout << "ERROR: cannot get axis range" << std::endl;
        exit(1);
    }
    m_axis_max = abs_info.maximum;
    m_axis_min = abs_info.minimum;
    if (m_axis_min >= m_axis_max) {
        std::cout << "ERROR: axis range has bad value" << std::endl;
        exit(1);
    }

    // check force feedback is supported?
    if(!testBit(FF_CONSTANT, ff_bits)) {
        std::cout << "ERROR: force feedback is not supported" << std::endl;
        exit(1);

    } else { std::cout << "force feedback supported" << std::endl; }

    // auto centering off
    memset(&event, 0, sizeof(event));
    event.type = EV_FF;
    event.code = FF_AUTOCENTER;
    event.value = 0;
    if (write(m_device_handle, &event, sizeof(event)) != sizeof(event)) {
        std::cout << "failed to disable auto centering" << std::endl;
        exit(1);
    }

    // init effect and get effect id
    memset(&m_effect, 0, sizeof(m_effect));
    m_effect.type = FF_CONSTANT;
    m_effect.id = -1; // initial value
    m_effect.trigger.button = 0;
    m_effect.trigger.interval = 0;
    m_effect.replay.length = 0xffff;  // longest value
    m_effect.replay.delay = 0; // delay from write(...)
    m_effect.u.constant.level = 0;
    m_effect.direction = 0xC000;
    m_effect.u.constant.envelope.attack_length = 0;
    m_effect.u.constant.envelope.attack_level = 0;
    m_effect.u.constant.envelope.fade_length = 0;
    m_effect.u.constant.envelope.fade_level = 0;

    if (ioctl(m_device_handle, EVIOCSFF, &m_effect) < 0) {
        std::cout << "failed to upload m_effect" << std::endl;
        exit(1);
    }

    // start m_effect
    memset(&event, 0, sizeof(event));
    event.type = EV_FF;
    event.code = m_effect.id;
    event.value = 1;
    if (write(m_device_handle, &event, sizeof(event)) != sizeof(event)) {
        std::cout << "failed to start event" << std::endl;
        exit(1);
    }
}


// util for initDevice()
int G29ForceFeedback::testBit(int bit, unsigned char *array) {

    return ((array[bit / (sizeof(unsigned char) * 8)] >> (bit % (sizeof(unsigned char) * 8))) & 1);
}

ros_g29_force_feedback::msg::ForceControl G29ForceFeedback::buildMessage(bool is_centering, bool human_ctrl, double torque) {
    auto message = ros_g29_force_feedback::msg::ForceControl();
    message.is_centering = is_centering;
    message.human_control = human_ctrl;
    message.torque = torque;

    return message;
}


int main(int argc, char * argv[]){
    rclcpp::init(argc, argv);

    auto g29_ff = std::make_shared<G29ForceFeedback>();
    rclcpp::spin(g29_ff);

    rclcpp::shutdown();
    return 0;
}
