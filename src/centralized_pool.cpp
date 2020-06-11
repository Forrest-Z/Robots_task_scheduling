#include "ros/ros.h"
#include "robot_navigation/make_task.h"
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/GetPlan.h>
#include <vector>
#include "util.h"
#include "sql_client.h"
#include "time_transfer.h"
#include "task_process.h"
#include <string>

typedef struct{
    double distance = 0;
    double sec_diff = 0;
    int periority = 0;
    int    statisic_open_possibility = 0;
    double battery_level = 0;
    double cost;
}CostFunction;

class CentralizedPool{

public:
    CentralizedPool():sql_client(SQLClient::getInstance()),task_process(cost_vector){
        init();
        load_room_position();
        load_charging_station_position();
        task_process.create_random_tasks(TASK_NUM,ros::Time::now(),room_map);
    }

    void init(){
	    ros::Duration(1).sleep();
        ROS_INFO_STREAM("Current office time: "<<TimeTransfer::convert_to_office_time_string(ros::Time::now())<<
                        "\nSimulation time : "<<ros::Time::now().sec);
        task_server = nh.advertiseService("make_task",&CentralizedPool::process_robot_request,this);
        plan_client = nh.serviceClient<nav_msgs::GetPlan>("move_base/NavfnROS/make_plan");      
    }

    void load_room_position(){
       if(sql_client.query_rooms_position(room_map)>0){
           ROS_INFO_STREAM("load "<<room_map.size()<< " room positions");
       } else{
           ROS_INFO_STREAM("load room position failed");
           exit(1);
       }
    }

    void load_charging_station_position(){
       if(sql_client.query_charging_stations_position(station_map)>0){
           ROS_INFO_STREAM("load "<<station_map.size()<< " station positions");
       } else{
           ROS_INFO_STREAM("load charging station position failed");
           exit(1);
       }
    }

    double get_statistic_open_possibility(char room_id, ros::Time time){
        PossibilityTableRow table_row;
        DoorStatusListRow list_row;
        table_row.room_id = room_id;
        list_row.room_id = room_id;
        list_row.date_time = TimeTransfer::convert_to_office_time_string(time);
        sql_client.query_posibility_table_single_room(table_row,list_row);
        ROS_INFO_STREAM("room id "<<table_row.room_id<<"pos "<<table_row.statistuc_open_pos);
        return table_row.statistuc_open_pos;
    }
    
    double calculate_cost(EnterRoomTask* task,ros::Time cur_time, geometry_msgs::Pose robot_pos){
            CostFunction cost_function;
            // for each task, calculate time different
            cost_function.sec_diff = (task->goal.header.stamp - cur_time).sec;
            if(cost_function.sec_diff<0){
                ROS_INFO_STREAM("Task "<<task->task_id<<" is expired ");
                return -1;
            }

            // for each task, check open possibility
            cost_function.statisic_open_possibility = get_statistic_open_possibility(task->room_id,cur_time);
 
            // for each task, request distance from move base plan server
            nav_msgs::GetPlan make_plan_srv;
            make_plan_srv.request.start.pose= robot_pos;
            make_plan_srv.request.start.header = task->goal.header;
            make_plan_srv.request.goal= task->goal;
            make_plan_srv.request.tolerance = 1;
	        
            // request plan with start point and end point
            if(!plan_client.call(make_plan_srv)){
                ROS_INFO_STREAM("Failed to send request to make plan server");
                return -1;
            }
            // calculate distance
            std::vector<geometry_msgs::PoseStamped> &dists = make_plan_srv.response.plan.poses;

            if(dists.size()==0){
                ROS_DEBUG("Receive empty plan");
                return -1;
            };   
     
            for(size_t i = 1; i < dists.size();i++){
                cost_function.distance += sqrt(pow((dists[i].pose.position.x - dists[i-1].pose.position.x),2) + 
                                    pow((dists[i].pose.position.y - dists[i-1].pose.position.y),2));
            }       
             
            cost_function.cost = cost_function.distance + 0.2 * cost_function.sec_diff +
                         100 - cost_function.statisic_open_possibility
                         + cost_function.periority * 5 +  
                         + 100 - cost_function.battery_level;

            ROS_INFO_STREAM("available task room id: "<<task->room_id<<
                                " distance "<<cost_function.distance<<
                                " time difference "<<cost_function.sec_diff<<
                                " open possibility "<<cost_function.statisic_open_possibility<<
                                " periority "<< cost_function.periority <<
                                " battery level "<<cost_function.battery_level <<
                                " cost "<<cost_function.cost);  
            return cost_function.cost;
    }

    bool process_robot_request(robot_navigation::make_task::Request &req,robot_navigation::make_task::Response &res){
        ros::Time cur_time = ros::Time::now();
        ROS_INFO_STREAM("receive request from a robot.\n last task: "<<req.last_task<<
                        "\nbettery level "<<req.battery_level<<
                        "\ncurrent position "<<Util::pose_str(req.pose));
        
        if(!req.last_task.is_completed){
            // to do
        }else{
            ROS_INFO_STREAM("Update possibility table");
            DoorStatusListRow list_row;
            list_row.date_time = TimeTransfer::convert_to_office_time_string(cur_time); // transfer to office time and update
            list_row.door_status = req.last_task.door_status;
            list_row.room_id = req.last_task.room_id;
            sql_client.update_possibility_table(list_row);
        }

        ROS_INFO_STREAM("There are "<<cost_vector.size()<<" tasks");

        if(!cost_vector.size()){
            ROS_INFO("No available tasks in centralized pool");
            return false;
        }

        for(size_t i = 0; i < cost_vector.size(); i++){
            // calculate cost for each task
            cost_vector[i].second = calculate_cost(cost_vector[i].first,cur_time,req.pose);
        }
  
        // sort with task cost
        std::sort(cost_vector.begin(),cost_vector.end(),
            [](const std::pair<EnterRoomTask*,double> &p1, const std::pair<EnterRoomTask*,double> &p2){
                return p1.second>p2.second;
            }
        );    
        
        // response best task to robot
        res.best_task.goal = cost_vector.back().first->goal;
        res.best_task.is_completed = false;
        res.best_task.room_id = cost_vector.back().first->room_id;

         ROS_INFO_STREAM("Give robot the best task "<<res.best_task<<" cost "<<cost_vector.back().second);
       
        // remove this task
        delete cost_vector.back().first;
        cost_vector.pop_back();
 
        return true;
    }

    
private:
    ros::ServiceServer task_server;

    ros::ServiceClient plan_client;

    SQLClient sql_client;

    TaskProcess task_process;

    ros::NodeHandle nh;

    std::vector<std::pair<EnterRoomTask*,double>> cost_vector;

    std::map<char,geometry_msgs::Pose> room_map;
    std::map<char,geometry_msgs::Pose> station_map;
};

int main(int argc, char** argv){
    ros::init(argc,argv,"centralized_poor");

    CentralizedPool pool;

    SQLClient sql_client;
    
    ros::spin(); // block program
    
}
