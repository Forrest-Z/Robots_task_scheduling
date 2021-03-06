#include "cost_function.h"

CostCalculator::CostCalculator(ros::NodeHandle &nh):_nh(nh){
        _pc = _nh.serviceClient<nav_msgs::GetPlan>("/tb3_0/move_base/NavfnROS/make_plan"); 
}

void CostCalculator::LoadTaskWeight(TaskWeight &tw){
    _tw = tw;
    ROS_INFO("Task weight %.2f %.2f %.2f %.2f",_tw.wt_btr,tw.wt_wait,tw.wt_psb,tw.wt_pri);
}

void CostCalculator::LoadDoorWeight(DoorWeight &dw){
    _dw = dw;
    ROS_INFO("Door weight %.2f %.2f %.2f",_dw.wt_btr,_dw.wt_update,_dw.wt_psb);
}


void CostCalculator::CalculateChargingStationCost(ChargingStation& cs, geometry_msgs::Pose robotPose){
    double battery =  CalculateSimpleBatteryConsumption(robotPose,cs.pose);
    cs.cost = _cw.wt_remain * cs.remainingTime + _cw.wt_btr * battery;
    ROS_INFO("%d %.1f      %.3f       %.3f", cs.stationId,cs.remainingTime, battery,cs.cost);
}

void CostCalculator::CalculateLargeTasksCost(ros::Time now,LargeExecuteTask& t, geometry_msgs::Pose robotPose){
    CalculateComplexTrajectoryBatteryConsumption(robotPose,t);
    t.waitingTime = t.smallTasks.begin()->second.point.goal.header.stamp - now; 
    t.cost =  _tw.wt_btr/ t.smallTasks.size() * t.battery 
                + _tw.wt_wait  * t.waitingTime.toSec() 
                + _tw.wt_psb * t.openPossibility 
                + _tw.wt_pri * t.priority;
    // ROS_INFO("%d        %.3f   %.3f   %.3f  %d  %3f",t.taskId,t.battery,t.waitingTime.toSec(), t.openPossibility,t.priority,t.cost);

}

void CostCalculator::CalculateDoorCost(ros::Time now, Door& door, geometry_msgs::Pose robotPose){
    double battery =  CalculateSimpleBatteryConsumption(robotPose,door.pose);
    double timeSinceLastUpdate = now.toSec() - door.lastUpdate.toSec();      
    door.cost = _dw.wt_btr * battery + _dw.wt_psb * door.product_psb + _dw.wt_update * timeSinceLastUpdate;
    //  ROS_INFO("%d  %.3f     %.3f     %.3f  %.3f", door.doorId, battery,timeSinceLastUpdate,door.product_psb, door.cost);
}


void CostCalculator::CalculateComplexTrajectoryBatteryConsumption(geometry_msgs::Pose robotPose,LargeExecuteTask& lt){
    double battery = 0.0;
    geometry_msgs::Pose start;
    std::map<int,SmallExecuteTask>::iterator it = lt.smallTasks.begin();
    battery += CalculateSimpleBatteryConsumption(robotPose,it->second.point.goal.pose);
    start = it->second.point.goal.pose; // store last trajectory end point
    for( it++;it != lt.smallTasks.end();it++){
        battery += CalculateSimpleBatteryConsumption(start,it->second.point.goal.pose);
        start = it->second.point.goal.pose;
    }
    lt.battery = battery;
}

double CostCalculator::CalculateSimpleBatteryConsumption(geometry_msgs::Pose start, geometry_msgs::Pose end){
        double distance = 0,angle = 0,batteryConsumption = 0;
    // for each task, request distance from move base plan server
        nav_msgs::GetPlan make_plan_srv;
        make_plan_srv.request.start.pose= start;
        make_plan_srv.request.start.header.frame_id = "map";
        make_plan_srv.request.goal.pose = end;
        make_plan_srv.request.goal.header.frame_id = "map";
        make_plan_srv.request.tolerance = 1;
        
        // request plan with start point and end point
        if(!_pc.call(make_plan_srv)){
            ROS_INFO_STREAM("Failed to send request to make plan server");
            return -1;
        }
        // calculate distance
        std::vector<geometry_msgs::PoseStamped> &dists = make_plan_srv.response.plan.poses;

        if(dists.size()==0){
            ROS_INFO("ERROR Receive empty plan start (%.3f,%.3f) end (%.3f,%3f)",start.position.x,start.position.y,end.position.x,end.position.y);
            return -1;
        };   
    
        for(size_t i = 1; i < dists.size();i++){
            distance = sqrt(pow((dists[i].pose.position.x - dists[i-1].pose.position.x),2) + 
                                pow((dists[i].pose.position.y - dists[i-1].pose.position.y),2));
                                    
            angle = 2 * acos(dists[i].pose.orientation.w);
            batteryConsumption = batteryConsumption + 0.01 * distance + 0.001 * angle;
            //   ROS_INFO("From (%.3f,%.3f) to (%.3f,%) linear variation = %.3f, angle variation = %.3f, battery consum = %.3f",
            //          dists[i-1].pose.position.x,dists[i-1].pose.position.y,dists[i].pose.position.x,dists[i].pose.position.y,distance,angle,batteryConsumption);
        }   
        //ROS_INFO("From (%.3f,%.3f) to (%.3f,%3f) battery %.3f",start.position.x,start.position.y,end.position.x,end.position.y,batteryConsumption);
        return batteryConsumption;   
}
