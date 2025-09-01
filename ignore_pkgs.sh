for pkg in $(echo "
	robotiq
	robotiq_2f_140_gripper_visualization
	robotiq_3f_gripper_articulated_msgs
	robotiq_3f_gripper_control
	robotiq_3f_gripper_joint_state_publisher
	robotiq_3f_gripper_msgs
	robotiq_3f_gripper_visualization
	robotiq_ethercat
	robotiq_ft_sensor
	robotiq_modbus_rtu
	robotiq_modbus_tcp
	")
do
	echo "Ignoring: $pkg"
	touch robotiq/$pkg/CATKIN_IGNORE
done

