for pkg in $(echo "
	robotiq
	robotiq_2f_140_gripper_visualization
	robotiq_2f_c2_gripper_visualization
	robotiq_2f_gripper_action_server
	robotiq_3f_gripper_articulated_gazebo
	robotiq_3f_gripper_articulated_gazebo_plugins
	robotiq_3f_gripper_articulated_msgs
	robotiq_3f_gripper_control
	robotiq_3f_gripper_joint_state_publisher
	robotiq_3f_gripper_msgs
	robotiq_3f_gripper_visualization
	robotiq_3f_rviz
	robotiq_ethercat
	robotiq_ft_sensor
	robotiq_modbus_rtu
	robotiq_modbus_tcp
	")
do
	echo "Ignoring: $pkg"
	touch robotiq/$pkg/CATKIN_IGNORE
done
