import cv2
import numpy as np
import rclpy

from cv_bridge import CvBridge
from rclpy.node import Node
from sensor_msgs.msg import Image


class BagRemap(Node):
    WIDTH = 854
    HEIGHT = 480

    def __init__(self):
        super().__init__("bag_remap")

        self.bridge = CvBridge()

        self._init_pub_sub()

        self.semantic_image = None
        self.rgb_image = None

    def _init_pub_sub(self):
        self.create_subscription(
            Image,
            "/carla/ego_vehicle/semantic_segmentation_front/image",
            self._semantic_callback,
            10,
        )
        self.create_subscription(Image, "/driver_img_view", self._rgb_callback, 10)

        self.semantic_pub = self.create_publisher(Image, "/resized_semantic", 10)
        self.rgb_pub = self.create_publisher(Image, "/resized_rgb", 10)

    def run_step(self):
        if self.semantic_image is None or self.rgb_image is None:
            return

        self.semantic_pub.publish(self.semantic_image)
        self.rgb_pub.publish(self.rgb_image)

    def resize(self, image_msg: Image) -> Image:
        img = self.bridge.imgmsg_to_cv2(image_msg, desired_encoding="rgb8")
        new_img = image_resize(img, self.WIDTH)
        return self.bridge.cv2_to_imgmsg(new_img, "rgb8")

    def _rgb_callback(self, data: Image):
        self.rgb_image = self.resize(data)

    def _semantic_callback(self, data: Image):
        self.semantic_image = self.resize(data)


def image_resize(image, width=None, height=None):
    (h, w) = image.shape[:2]

    if width is None and height is None:
        return image

    if width is None:
        r = height / float(h)
        dim = (int(w * r), height)

    else:
        r = width / float(w)
        dim = (width, int(h * r))

    resized = cv2.resize(image, dim, interpolation=cv2.INTER_AREA)

    return resized


def main(args=None):
    rclpy.init(args=args)

    remap = BagRemap()

    try:
        remap.create_timer(0.1, lambda _=None: remap.run_step())

        rclpy.spin(remap)
    except KeyboardInterrupt:
        pass

    remap.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
