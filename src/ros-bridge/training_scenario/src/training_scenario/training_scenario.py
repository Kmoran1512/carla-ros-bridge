import random
import csv
import carla
import carla_common.transforms as trans
import rclpy

from rclpy.node import Node
from carla_msgs.srv import SpawnObject
from carla_msgs.msg import CarlaTrafficLightStatusList, CarlaWeatherParameters
from geometry_msgs.msg import Pose, PoseArray, PoseStamped
from typing import List

import time

random.seed(4242)


class TrainingScenario(Node):
    def __init__(self):
        super(TrainingScenario, self).__init__("TrainingScenario")

        self._init_params()
        self._init_carla()
        self._init_pub_sub()

        self.walkers = {}
        self.walker_agents = {}

        self._preset_points = []
        self._speeds = []
        self.goals = self._init_goals()

        self.start = time.time()

    def _init_params(self):
        self.declare_parameter("pedestrian_number", "1")
        self.declare_parameter("sun_azimuth", "60.0")
        self.declare_parameter("sun_elevation", "2.0")
        self.declare_parameter("spawn_definition_file", "")
        self.declare_parameter("speeds_file", "")

        self.pedestrian_number = int(self.get_parameter("pedestrian_number").value)
        self.azimuth = float(self.get_parameter("sun_azimuth").value)
        self.altitude = float(self.get_parameter("sun_elevation").value)
        self.spawn_definition_file = self.get_parameter("spawn_definition_file").value
        self.speeds_file = self.get_parameter("speeds_file").value

    def _init_carla(self):
        self.client = carla.Client("localhost", 2000)
        self.client.set_timeout(5.0)
        self.world = self.client.get_world()
        self.world.set_pedestrians_cross_factor(0.7)

    def _init_pub_sub(self):
        self.spawn_actors_service = self.create_client(
            SpawnObject, "/carla/spawn_object"
        )

        self.create_subscription(
            CarlaTrafficLightStatusList,
            "/carla/traffic_lights/status",
            self.traffic_light_status,
            10,
        )
        self.create_subscription(
            PoseArray, "/carla/ego_vehicle/next_50", self._waypoints_updated, 10
        )

        self.weather_publisher = self.create_publisher(
            CarlaWeatherParameters, "/carla/weather_control", 10
        )
        self.goal_publisher = self.create_publisher(
            PoseStamped, "/carla/ego_vehicle/goal", 10
        )

    def _init_goals(self) -> List[PoseStamped]:
        self.goal_lock = True

        new_goals = [
            PoseStamped(),
            PoseStamped(),
            PoseStamped(),
            PoseStamped(),
            PoseStamped(),
        ]
        new_goals[0].pose.position.x = -1.5
        new_goals[0].pose.position.y = -15.0
        new_goals[1].pose.position.x = 317.5
        new_goals[1].pose.position.y = -55.0
        new_goals[2].pose.position.x = 305.0
        new_goals[2].pose.position.y = -136.5
        new_goals[3].pose.position.x = 92.5
        new_goals[3].pose.position.y = -309.5
        new_goals[4].pose.position.x = 92.5
        new_goals[4].pose.position.y = -20.5

        return new_goals

    def clean_up(self):
        self.client.apply_batch(
            [carla.command.DestroyActor(x) for x in self.walker_agents.keys()]
        )

    def run_step(self):
        if len(self.walker_agents) == 0:
            return
        self.world.wait_for_tick()

    def set_weather(self):
        weather_msg = CarlaWeatherParameters()
        weather_msg.sun_azimuth_angle = self.azimuth
        weather_msg.sun_altitude_angle = self.altitude
        self.weather_publisher.publish(weather_msg)

    def spawn_walkers(self):
        if not self.spawn_actors_service.wait_for_service(timeout_sec=10.0):
            self.get_logger().error("Service spawn_actors not available after waiting")
            return

        self._get_config(rand=True)
        walker_requests = []
        for i in range(self.pedestrian_number):
            walker_request = SpawnObject.Request()

            number = (i % 49) + 1

            walker_request.type = f"walker.pedestrian.{number:04}"
            walker_request.id = f"walker{i:04}"
            walker_request.transform = self._preset_points[i]

            walker_requests.append(self.spawn_actors_service.call_async(walker_request))

        for i, request in enumerate(walker_requests):
            future = request
            rclpy.spin_until_future_complete(self, future)
            walker_id = future.result().id
            if walker_id > 0:
                self.walkers[walker_id] = self.world.get_actor(walker_id)

        self.get_logger().info(f"\n\n\n spawned {len(self.walkers)} walkers \n\n\n")
        self._walk()

    def traffic_light_status(self, traffic_light_info_msg):
        lights_on = []
        for tl_info in traffic_light_info_msg.traffic_lights:
            if tl_info.state == 3:
                continue
            lights_on.append(tl_info.id)

        if not lights_on:
            return
        all_actors = self.world.get_actors(lights_on)
        self._turn_off_lights(all_actors)

    def _waypoints_updated(self, data):
        if len(data.poses) == 50:
            self.goal_lock = False

        if self.goal_lock:
            return
        elif len(self.goals) == 0 or len(data.poses) > 5:
            return

        self.goal_lock = True
        self.goal_publisher.publish(self.goals.pop())

    def _get_config(self, rand=False):
        if rand:
            for _ in range(self.pedestrian_number):
                loc = self.world.get_random_location_from_navigation()
                if loc == None:
                    continue

                pt = Pose()
                pt.position = trans.carla_location_to_ros_point(loc)
                self._preset_points.append(pt)
        else:
            with open(self.spawn_definition_file, newline="") as f:
                csv_points = [tuple(row) for row in csv.reader(f, delimiter=" ")]

            for x, y, z in csv_points:
                pt = Pose()
                pt.position.x = float(x)
                pt.position.y = -float(y)
                pt.position.z = float(z)

                self._preset_points.append(pt)

        with open(self.speeds_file, newline="") as f:
            self._speeds = [float(row[0]) for row in csv.reader(f)]

    def _turn_off_lights(self, light_actors):
        for actor in light_actors:
            if actor.__class__ is not carla.libcarla.TrafficLight:
                continue
            actor.set_state(carla.libcarla.TrafficLightState.Off)
            actor.freeze(True)

    def _walk(self):
        walker_controller_bp = self.world.get_blueprint_library().find(
            "controller.ai.walker"
        )

        batch = []
        for walker_id in self.walkers.keys():
            batch.append(
                carla.command.SpawnActor(
                    walker_controller_bp, carla.Transform(), walker_id
                )
            )

        results = self.client.apply_batch_sync(batch, True)
        for result in results:
            if result.error:
                self.get_logger().error(result.error)
            else:
                self.walker_agents[result.actor_id] = self.world.get_actor(
                    result.actor_id
                )

        for i, agent in enumerate(self.walker_agents.values()):
            agent.start()
            pose_in_map = random.choice(self._preset_points)
            loc = trans.ros_point_to_carla_location(pose_in_map.position)
            agent.go_to_location(loc)
            agent.set_max_speed(self._speeds[i % 49])

        self.get_logger().info(
            f"\n\n\n Agents spawned {len(self.walker_agents)} \n\n\n"
        )


def main():
    rclpy.init()

    ts = TrainingScenario()
    ts.spawn_walkers()
    ts.set_weather()

    try:
        ts.create_timer(0.05, lambda timer_event=None: ts.run_step())

        rclpy.spin(ts, ts.executor)
    except KeyboardInterrupt:
        pass

    ts.clean_up()
    ts.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
