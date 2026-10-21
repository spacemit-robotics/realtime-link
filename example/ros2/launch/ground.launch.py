"""启动地面端桥接节点，保留可选采集配置参数。"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    default_config = PathJoinSubstitution(
        [FindPackageShare("fpv_video"), "config", "ground.yaml"]
    )
    return LaunchDescription([
        DeclareLaunchArgument("config", default_value=default_config),
        DeclareLaunchArgument("camera_config", default_value=LaunchConfiguration("config")),
        Node(
            package="fpv_video_bridge",
            executable="fpv_bridge",
            name="fpv_ground",
            parameters=[{
                "config": LaunchConfiguration("config"),
                "camera_config": LaunchConfiguration("camera_config"),
                "role": "ground",
            }],
            output="screen",
        ),
    ])