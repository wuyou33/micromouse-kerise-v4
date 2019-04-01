#pragma once

#include "config/model.h"
#include "global.h"

#include "TrajectoryTracker.h"
#include "config/trajectory.h"

#include "TaskBase.h"
#include <AccelDesigner.h>
#include <cmath>
#include <queue>
#include <vector>

#define SEARCH_WALL_ATTACH_ENABLED 1
#define SEARCH_WALL_CUT_ENABLED 1
#define SEARCH_WALL_FRONT_ENABLED 0
#define SEARCH_WALL_AVOID_ENABLED 1

#define SEARCH_RUN_TASK_PRIORITY 3
#define SEARCH_RUN_STACK_SIZE 8192

static constexpr const float ahead_length = 0.0f;

#define SEARCH_RUN_VELOCITY 240.0f
#define SEARCH_RUN_V_MAX 600.0f

class SearchRun : TaskBase {
public:
  enum ACTION {
    START_STEP,
    START_INIT,
    GO_STRAIGHT,
    GO_HALF,
    TURN_LEFT_90,
    TURN_RIGHT_90,
    TURN_BACK,
    RETURN,
    STOP,
  };
  const char *action_string(enum ACTION action) {
    static const char name[][32] = {
        "start_step",    "start_init", "go_straight", "go_half", "turn_left_90",
        "turn_right_90", "turn_back",  "return",      "stop",
    };
    return name[action];
  }
  struct Operation {
    enum ACTION action;
    int num;
  };
#ifndef M_PI
  static constexpr float M_PI = 3.14159265358979323846f;
#endif

public:
  SearchRun() {}
  virtual ~SearchRun() {}
  void enable() {
    deleteTask();
    createTask("SearchRun", SEARCH_RUN_TASK_PRIORITY, SEARCH_RUN_STACK_SIZE);
  }
  void disable() {
    deleteTask();
    sc.disable();
    while (q.size()) {
      q.pop();
    }
  }
  void set_action(enum ACTION action, int num = 1) {
    struct Operation operation;
    operation.action = action;
    operation.num = num;
    q.push(operation);
    isRunningFlag = true;
  }
  bool isRunning() { return isRunningFlag; }
  //   int actions() const { return q.size(); }
  //   void waitForEnd() const { xSemaphoreTake(wait, portMAX_DELAY); }
  void printPosition(const char *name) const {
    printf("%s\tRel:(%.1f, %.1f, %.1f)\n", name, sc.position.x, sc.position.y,
           sc.position.th * 180 / PI);
  }
  bool positionRecovery() {
    sc.enable();
    for (int i = 0; i < 4; ++i) {
      if (wd.wall[2])
        wall_attach(true);
      turn(PI / 2);
    }
    while (1) {
      if (!wd.wall[2])
        break;
      wall_attach();
      turn(-PI / 2);
    }
    sc.disable();
    return true;
  }

private:
  std::queue<struct Operation> q;
  volatile bool isRunningFlag = false;
  bool prev_wall[2];

  void wall_attach(bool force = false) {
#if SEARCH_WALL_ATTACH_ENABLED
    if ((force && tof.getDistance() < 180) || tof.getDistance() < 90 ||
        (wd.distance.front[0] > 10 && wd.distance.front[1] > 10)) {
      tof.disable();
      portTickType xLastWakeTime = xTaskGetTickCount();
      SpeedController::WheelParameter wi;
      for (int i = 0; i < 3000; i++) {
        const float Kp = 72.0f;
        const float Ki = 6.0f;
        const float satu = 120.0f; //< [mm/s]
        const float end = 0.4f;
        SpeedController::WheelParameter wp;
        for (int j = 0; j < 2; ++j) {
          wp.wheel[j] = -wd.distance.front[j];
          wi.wheel[j] += wp.wheel[j] * 0.001f * Ki;
          wp.wheel[j] = wp.wheel[j] * Kp + wi.wheel[j] * Ki;
          wp.wheel[j] = std::max(std::min(wp.wheel[j], satu), -satu);
        }
        if (std::abs(wp.wheel[0]) + std::abs(wp.wheel[1]) < end)
          break;
        wp.wheel2pole();
        sc.set_target(wp.tra, wp.rot);
        vTaskDelayUntil(&xLastWakeTime, 1 / portTICK_RATE_MS);
      }
      sc.set_target(0, 0);
      sc.position.x = 0;  //< 直進方向の補正
      sc.position.th = 0; //< 回転方向の補正
      tof.enable();
      bz.play(Buzzer::SHORT);
    }
#endif
  }
  void wall_avoid(const float distance) {
#if SEARCH_WALL_AVOID_ENABLED
    if (std::abs(sc.position.th) < 0.05 * PI) {
      const float gain = 0.002;
      if (wd.wall[0])
        sc.position.y += wd.distance.side[0] * gain;
      if (wd.wall[1])
        sc.position.y -= wd.distance.side[1] * gain;
    }
#endif
#if SEARCH_WALL_CUT_ENABLED
#define SEARCH_WALL_CUT_OFFSET_X_ 66
    for (int i = 0; i < 2; i++) {
      if (prev_wall[i] && !wd.wall[i] && sc.position.x > 30.0f) {
        const float prev_x = sc.position.x;
        float fix = -((int)sc.position.x) % SEGMENT_WIDTH +
                    SEARCH_WALL_CUT_OFFSET_X_ - ahead_length;
        if (distance > SEGMENT_WIDTH - 1 && fix < 0.0f) {
          sc.position.x = sc.position.x + fix;
          printf("WallCut[%d] X_ distance: %.0f, x: %.1f => %.1f\n", i,
                 distance, prev_x, sc.position.x);
          bz.play(Buzzer::CANCEL);
        }
      }
      prev_wall[i] = wd.wall[i];
    }
#endif
  }
  void wall_calib(const float velocity) {
#if SEARCH_WALL_FRONT_ENABLED
    if (wd.wall[2]) {
      float value =
          tof.getDistance() - (5 + tof.passedTimeMs()) / 1000.0f * velocity;
      float x = sc.position.x;
      if (value > 60 && value < 120) {
        sc.position.x = 90 - value - ahead_length;
        bz.play(Buzzer::SELECT);
      }
      sc.position.x = std::min(sc.position.x, 0.0f);
      printf("FrontWallCalib: %.2f => %.2f\n", x, sc.position.x);
    }
#endif
  }
  void turn(const float angle, bool fix = false) {
    const float speed = 3 * M_PI;
    const float accel = 24 * M_PI;
    const float decel = 24 * M_PI;
    const float back_gain = 1.0f;
    int ms = 0;
    portTickType xLastWakeTime = xTaskGetTickCount();
    while (1) {
      if (fix)
        break;
      if (std::abs(sc.est_v.rot) > speed)
        break;
      float delta = sc.position.x * std::cos(-sc.position.th) -
                    sc.position.y * std::sin(-sc.position.th);
      if (angle > 0) {
        sc.set_target(-delta * back_gain, ms / 1000.0f * accel);
      } else {
        sc.set_target(-delta * back_gain, -ms / 1000.0f * accel);
      }
      vTaskDelayUntil(&xLastWakeTime, 1 / portTICK_RATE_MS);
      xLastWakeTime = xTaskGetTickCount();
      ms++;
    }
    while (1) {
      vTaskDelayUntil(&xLastWakeTime, 1 / portTICK_RATE_MS);
      xLastWakeTime = xTaskGetTickCount();
      float extra = angle - sc.position.th;
      if (std::abs(sc.est_v.rot) < 0.1 && std::abs(extra) < 0.1)
        break;
      float target_speed = sqrt(2 * decel * std::abs(extra));
      float delta = sc.position.x * std::cos(-sc.position.th) -
                    sc.position.y * std::sin(-sc.position.th);
      target_speed = (target_speed > speed) ? speed : target_speed;
      if (extra > 0) {
        sc.set_target(-delta * back_gain, target_speed);
      } else {
        sc.set_target(-delta * back_gain, -target_speed);
      }
    }
    sc.set_target(0, 0);
    sc.position.th -= angle; //< 移動した量だけ位置を更新
    sc.position = sc.position.rotate(-angle); //< 移動した量だけ位置を更新
    printPosition("Turn End");
  }
  void straight_x(const float distance, const float v_max, const float v_end) {
    const float jerk = 500000;
    const float accel = 6000;
    const float v_start = sc.ref_v.tra;
    TrajectoryTracker tt(tt_gain);
    tt.reset(v_start);
    portTickType xLastWakeTime = xTaskGetTickCount();
    AccelDesigner ad(jerk, accel, v_start, v_max, v_end, distance);
    float int_y = 0;
    for (float t = 0; t < ad.t_end(); t += 0.001f) {
      auto est_q = sc.position;
      auto ref_q = Position(ad.x(t), 0);
      auto ref_dq = Position(ad.v(t), 0);
      auto ref_ddq = Position(ad.a(t), 0);
      auto ref_dddq = Position(ad.j(t), 0);
      auto ref = tt.update(est_q, sc.est_v, sc.est_a, ref_q, ref_dq, ref_ddq,
                           ref_dddq);
      sc.set_target(ref.v, ref.w, ref.dv, ref.dw);
      vTaskDelayUntil(&xLastWakeTime, 1 / portTICK_RATE_MS);
      wall_avoid(distance);
      int_y += sc.position.y;
      sc.position.th += int_y * 0.00000001f;
    }
    if (v_end < 1.0f)
      sc.set_target(0, 0);
    sc.position.x -= distance; //< 移動した量だけ位置を更新
  }
  void trace(SlalomDesigner &sd, const float velocity) {
    TrajectoryTracker tt(tt_gain);
    tt.reset(velocity);
    SlalomDesigner::State s;
    const float Ts = 0.001f;
    sd.reset(velocity);
    portTickType xLastWakeTime = xTaskGetTickCount();
    for (float t = 0; t < sd.t_end(); t += 0.001f) {
      sd.update(&s, Ts);
      auto est_q = sc.position;
      auto ref = tt.update(est_q, sc.est_v, sc.est_a, s.q, s.dq, s.ddq, s.dddq);
      sc.set_target(ref.v, ref.w, ref.dv, ref.dw);
      vTaskDelayUntil(&xLastWakeTime, 1 / portTICK_RATE_MS);
    }
    sc.set_target(velocity, 0);
    const auto net = sd.get_net_curve();
    sc.position = (sc.position - net).rotate(-net.th);
  }
  void put_back() {
    const int max_v = 150;
    for (int i = 0; i < max_v; i++) {
      sc.set_target(-i, -sc.position.th * 200.0f);
      delay(1);
    }
    for (int i = 0; i < 100; i++) {
      sc.set_target(-max_v, -sc.position.th * 200.0f);
      delay(1);
    }
    sc.disable();
    mt.drive(-0.1f, -0.1f);
    delay(200);
    mt.drive(-0.2f, -0.2f);
    delay(200);
    sc.enable(true);
  }
  void uturn() {
    if (wd.distance.side[0] < wd.distance.side[1]) {
      wall_attach();
      turn(-M_PI / 2);
      wall_attach();
      turn(-M_PI / 2);
    } else {
      wall_attach();
      turn(M_PI / 2);
      wall_attach();
      turn(M_PI / 2);
    }
  }
  void stop() {
    // bz.play(Buzzer::EMERGENCY);
    bz.play(Buzzer::ERROR);
    float v = sc.est_v.tra;
    while (v > 0) {
      sc.set_target(v, 0);
      v -= 9;
      delay(1);
    }
    sc.disable();
    mt.emergencyStop();
    vTaskDelay(portMAX_DELAY);
  }
  void task() override {
    const float velocity = SEARCH_RUN_VELOCITY;
    const float v_max = SEARCH_RUN_V_MAX;
    // スタート
    sc.enable();
    while (1) {
      //** SearchActionがキューされるまで直進で待つ
      if (q.empty())
        isRunningFlag = false;
      {
        float v = velocity;
        portTickType xLastWakeTime = xTaskGetTickCount();
        while (q.empty()) {
          vTaskDelayUntil(&xLastWakeTime, 1 / portTICK_RATE_MS);
          xLastWakeTime = xTaskGetTickCount();
          Position cur = sc.position;
#define SEARCH_ST_LOOK_AHEAD(v) (5 + 20 * v / 240)
#define SEARCH_ST_FB_GAIN 40
          float th = atan2f(-cur.y, SEARCH_ST_LOOK_AHEAD(velocity)) - cur.th;
          sc.set_target(v, SEARCH_ST_FB_GAIN * th);
          wall_avoid(0);
        }
      }
      struct Operation operation = q.front();
      q.pop();
      while (!q.empty()) {
        auto next = q.front();
        if (operation.action != next.action)
          break;
        operation.num += next.num;
        q.pop();
      }
      enum ACTION action = operation.action;
      int num = operation.num;
      printf("Action: %d %s\n", num, action_string(action));
      printPosition("Start");
      switch (action) {
      case START_STEP:
        sc.position.clear();
        imu.angle = 0;
        straight_x(SEGMENT_WIDTH - MACHINE_TAIL_LENGTH - WALL_THICKNESS / 2 +
                       ahead_length,
                   velocity, velocity);
        break;
      case START_INIT:
        straight_x(SEGMENT_WIDTH / 2 - ahead_length, velocity, 0);
        wall_attach();
        turn(M_PI / 2);
        wall_attach();
        turn(M_PI / 2);
        put_back();
        mt.free();
        isRunningFlag = false;
        vTaskDelay(portMAX_DELAY);
      case GO_STRAIGHT:
        if (wd.wall[2])
          stop();
        straight_x(SEGMENT_WIDTH * num, num > 1 ? v_max : velocity, velocity);
        break;
      case GO_HALF:
        straight_x(SEGMENT_WIDTH / 2 * num, velocity, velocity);
        break;
      case TURN_LEFT_90:
        for (int i = 0; i < num; i++) {
          if (wd.wall[0])
            stop();
          wall_calib(velocity);
          straight_x(sd_SL90.get_straight_prev() - ahead_length, velocity,
                     velocity);
          trace(sd_SL90, velocity);
          straight_x(sd_SL90.get_straight_post() + ahead_length, velocity,
                     velocity);
        }
        break;
      case TURN_RIGHT_90:
        for (int i = 0; i < num; i++) {
          if (wd.wall[1])
            stop();
          wall_calib(velocity);
          straight_x(sd_SR90.get_straight_prev() - ahead_length, velocity,
                     velocity);
          trace(sd_SR90, velocity);
          straight_x(sd_SR90.get_straight_post() + ahead_length, velocity,
                     velocity);
        }
        break;
      case TURN_BACK:
        straight_x(SEGMENT_WIDTH / 2 - ahead_length, velocity, 0);
        uturn();
        straight_x(SEGMENT_WIDTH / 2 + ahead_length, velocity, velocity);
        break;
      case RETURN:
        uturn();
        break;
      case STOP:
        straight_x(SEGMENT_WIDTH / 2 - ahead_length, velocity, 0);
        // wall_attach();
        turn(0, true);
        sc.disable();
        isRunningFlag = false;
        vTaskDelay(portMAX_DELAY);
        break;
      }
      printPosition("End");
    }
    vTaskDelay(portMAX_DELAY);
  }
};
