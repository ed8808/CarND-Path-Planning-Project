#include <uWS/uWS.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "Eigen-3.3/Eigen/LU"
#include "helpers.h"
#include "json.hpp"
#include "spline.h"

// for convenience
using nlohmann::json;
using std::string;
using std::vector;
using Eigen::MatrixXd;
using Eigen::VectorXd;

int lane=1;

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;
  
  std::ifstream in_map_(map_file_.c_str(), std::ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    std::istringstream iss(line);
    double x;
    double y;
    float s;
    float d_x;
    float d_y;
    iss >> x;
    iss >> y;
    iss >> s;
    iss >> d_x;
    iss >> d_y;
    map_waypoints_x.push_back(x);
    map_waypoints_y.push_back(y);
    map_waypoints_s.push_back(s);
    map_waypoints_dx.push_back(d_x);
    map_waypoints_dy.push_back(d_y);
  }
    
  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,
               &map_waypoints_dx,&map_waypoints_dy]
              (uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
               uWS::OpCode opCode) {
                
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
          // j[1] is the data JSON object
          
          // Main car's localization Data
          double car_x = j[1]["x"];
          double car_y = j[1]["y"];
          double car_s = j[1]["s"];
          double car_d = j[1]["d"];
          double car_yaw = j[1]["yaw"];
          double car_speed = j[1]["speed"];
          
          // Previous path data given to the Planner
          auto previous_path_x = j[1]["previous_path_x"];
          auto previous_path_y = j[1]["previous_path_y"];
          // Previous path's end s and d values 
          double end_path_s = j[1]["end_path_s"];
          double end_path_d = j[1]["end_path_d"];

		  /*################################################################################################################*/
		  /*		Sensor Fusion for other traffic																			*/
		  /*################################################################################################################*/
          // Sensor Fusion Data, a list of all other cars on the same side 
          //   of the road.
          auto sensor_fusion = j[1]["sensor_fusion"];
          bool lane_change=0;
		  double max_vel = 49.5;
          double min_vel = 1;
  		  double ref_vel;
          double safety = 30;
          int path_size = previous_path_x.size();

		  vector <double> carss_s;
          vector <double> carss_d;
          vector <double> carss_vx;
		  
		  /* Sensor Fusion Here */
          for(int i=0;i<sensor_fusion.size();i++)
          {
            int cars_id = sensor_fusion[i][0];
            double cars_x = sensor_fusion[i][1];
            double cars_y = sensor_fusion[i][2];
            double cars_vx = sensor_fusion[i][3];
            double cars_vy = sensor_fusion[i][4];
            double cars_s = sensor_fusion[i][5];
            double cars_d = sensor_fusion[i][6];
            
            carss_s.push_back(cars_s);
            carss_d.push_back(cars_d);
            carss_vx.push_back(cars_vx);   
          }

          vector <double> cars_close={999,999,999,999,999,999};
          get_cars_approach(car_s, car_speed, carss_s, carss_d, carss_vx, cars_close);
          //for(int n : cars_close) {std::cout << n << " ";} std::cout << "\n";
          double t;
          
		  // calculate distances among front cars in all lanes
          if(car_d - 2 < 1) t = cars_close[0];
          else if(car_d - 6 < 1) t = cars_close[2];
          else if(car_d - 10 < 1) t = cars_close[4];
             
		  
		  /* First detect if car in front is close or far away */
          if(t < safety*2)
          {               
			//closing up: safety distance < threshold, lane change initiated, keep car speed
            lane_change=1;
          }
          else
          {
			//far away: increment reference velocity +1 if no closing by car in front
            ref_vel += min_vel;
            lane_change=0;
          }
          
		  // lane change state machine
          if(lane_change)
          {
            if(abs(car_d - 6) < 1)
            {
              if(cars_close[0] > cars_close[2] && cars_close[1] > safety/2)
			  {
				//left lane front car is further away and left lane back car is not yet closing in, reduce speed to mid-lane front car not to reduce acceleration during lane change
                ref_vel -= min_vel;
                if(ref_vel < cars_close[2]) ref_vel = cars_close[2];
                lane = 0;
              }
              else if(cars_close[4] > cars_close[2]  && cars_close[5] > safety/2)
			  {
				//right lane front car is further away and right lane back car is not yet closing in, reduce speed to mid-lane front car not to reduce acceleration during lane change
                ref_vel -= min_vel;
                if(ref_vel < cars_close[2]) ref_vel = cars_close[2];
                lane = 2;        
              }
              else if(t < safety)
              {
				//front car is too close but unable to change lanes, reduce speed
                ref_vel -= min_vel;
                if(ref_vel < cars_close[2]) ref_vel = cars_close[2];
              }
            }
            else if(abs(car_d - 2) < 1)
            {
			  //middle lane front car is further away and middle lane back car is not yet closing in, reduce speed to left front car not to reduce acceleration during lane change
              if(cars_close[2] > cars_close[0]  && cars_close[3] > safety/2){
                ref_vel -= min_vel;
                if(ref_vel < cars_close[0]) ref_vel = cars_close[0];
                lane = 1;
              }
              else if(t < safety)
              {
				//front car is too close but unable to change lanes, reduce speed
                ref_vel -= min_vel;
                if(ref_vel < cars_close[0]) ref_vel = cars_close[0];
              }
            }
            else if(abs(car_d - 10) < 1)
            {
			  //middle lane front car is further away and middle lane back car is not yet closing in, reduce speed to right lane front car not to reduce acceleration during lane change
              if(cars_close[2] > cars_close[4]  && cars_close[3] > safety/2){
                ref_vel -= min_vel;
                if(ref_vel < cars_close[4]) ref_vel = cars_close[4];
                lane = 1;
              }
              else if(t < safety)
              {
				//front car is too close but unable to change lanes, reduce speed
                ref_vel -= min_vel;
                if(ref_vel < cars_close[4]) ref_vel = cars_close[4];
              }
            }
          }
          
		  // bound min. and max. reference velocities
          if(ref_vel < min_vel) ref_vel = min_vel;
          if(ref_vel > max_vel) ref_vel = max_vel;
          
		  /*################################################################################################################*/
		  /*		Path Planning																							*/
		  /*################################################################################################################*/
          vector <double> ptsx, ptsy;         
          double ref_x, ref_y, ref_yaw, ref_x_prev, ref_y_prev, ref_yaw_prev;
          ref_x = car_x;
          ref_y = car_y;
          ref_yaw = deg2rad(car_yaw);
          ref_x_prev = ref_x - cos(ref_yaw);
          ref_y_prev = ref_y - sin(ref_yaw);
          
          if (path_size < 2) {
			// start from minimum velocity
            ref_vel = min_vel;
          }
          else{
            ref_x = previous_path_x[path_size-1];
            ref_y = previous_path_y[path_size-1];
            ref_x_prev = previous_path_x[path_size-2];
            ref_y_prev = previous_path_y[path_size-2];
            ref_yaw = atan2((ref_y-ref_y_prev), (ref_x-ref_x_prev));
          }
          
		  // Get 2 last XYs
          ptsx.push_back(ref_x_prev);
          ptsy.push_back(ref_y_prev);
          ptsx.push_back(ref_x);
          ptsy.push_back(ref_y);

		  // Get 3 projected Frenet at 50, 70 and 90 further away from the car
          vector <double> next_wp0 = getXY(car_s+50, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector <double> next_wp1 = getXY(car_s+70, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector <double> next_wp2 = getXY(car_s+90, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          
          ptsx.push_back(next_wp0[0]);
          ptsx.push_back(next_wp1[0]);
          ptsx.push_back(next_wp2[0]);
          
          ptsy.push_back(next_wp0[1]);
          ptsy.push_back(next_wp1[1]);
          ptsy.push_back(next_wp2[1]);
          
		  // transform all XYs to car coordinates
          for(int i=0;i<ptsx.size();i++)
          {
            double shift_x = ptsx[i]-ref_x;
            double shift_y = ptsy[i]-ref_y;		
            ptsx[i] = (shift_x * cos(0-ref_yaw) - shift_y * sin(0-ref_yaw));
            ptsy[i] = (shift_x * sin(0-ref_yaw) + shift_y * cos(0-ref_yaw));
          }
          // apply SPLINE to fit 5 pts   
          tk::spline s; 
          s.set_points(ptsx, ptsy);
          
		  // save all last unprocessed XYs
          vector<double> next_x_vals;
          vector<double> next_y_vals;
          for(int i=0; i<path_size; i++)
          {
            next_x_vals.push_back(previous_path_x[i]);
            next_y_vals.push_back(previous_path_y[i]);
          }         
          
          double target_x = 10.0;
          double target_y = s(target_x);
          double target_dist = sqrt((target_x)*(target_x)+(target_y)*(target_y));
          
          double x_add_on = 0;            
          
		  // generate all remaining points to reach 50 pts from last SPLINE
          for(int i=1;i<=50-path_size;i++)
          {  
            double N = (target_dist / (.02 * ref_vel/2.24));
            double x_point = x_add_on + target_x/N;
            double y_point = s(x_point);
            
            x_add_on = x_point;
            
            double x_ref = x_point;
            double y_ref = y_point;
            
			// transform back from the car to map coordinates
            x_point = (x_ref * cos(ref_yaw) - y_ref * sin(ref_yaw));
            y_point = (x_ref * sin(ref_yaw) + y_ref * cos(ref_yaw));
            
            x_point += ref_x;
            y_point += ref_y;
            
            next_x_vals.push_back(x_point);
            next_y_vals.push_back(y_point);
          }

		  json msgJson;
          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }  // end "telemetry" if
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }  // end websocket if
  }); // end h.onMessage

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  
  h.run();
}