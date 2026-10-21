"""启动天空端桥接节点，分别传递会话与采集配置。"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    default_config = PathJoinSubstitution(
        [FindPackageShare("fpv_video"), "config", "sky.yaml"]
    )
    return LaunchDescription([
        DeclareLaunchArgument("config", default_value=default_config),
        DeclareLaunchArgument("camera_config", default_value=LaunchConfiguration("config")),
        Node(
            package="fpv_video_bridge",
            executable="fpv_bridge",
            name="fpv_sky",
            parameters=[{
                "config": LaunchConfiguration("config"),
                "camera_config": LaunchConfiguration("camera_config"),
                "role": "sky",
            }],
            output="screen",
        ),
    ])