#include <Eigen/Geometry>
#include <args.hxx>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <lodepng.h>
#include <memory>
#include <random>
#include <robot_design/glfw_viewer.h>
#include <robot_design/graph.h>
#include <robot_design/optim.h>
#include <robot_design/render.h>
#include <robot_design/robot.h>
#include <robot_design/sim.h>
#include <thread>
#include <vector>

using namespace robot_design;

int main(int argc, char **argv) {
  args::ArgumentParser parser("Robot design graph viewer.");
  args::HelpFlag help_flag(parser, "help", "Display this help message",
                           {'h', "help"});
  args::Positional<std::string> graph_file_arg(
      parser, "graph_file", "Graph file (.dot)", args::Options::Required);
  args::PositionalList<unsigned int> rule_sequence_arg(
      parser, "rule_sequence", "Rule sequence to apply");
  args::ValueFlag<unsigned int> seed_flag(parser, "seed", "Random seed",
                                          {'s', "seed"});
  args::ValueFlag<unsigned int> jobs_flag(
      parser, "jobs", "Number of jobs/threads", {'j', "jobs"}, 0);
  args::ValueFlag<unsigned int> episodes_flag(
      parser, "episodes", "Number of episodes", {'e', "episodes"}, 3);
  args::Flag optim_flag(parser, "optim", "Optimize a trajectory",
                        {'o', "optim"});
  args::Flag render_flag(parser, "render", "Render the trajectory",
                         {'r', "render"});
  args::ValueFlag<std::string> save_image_flag(
      parser, "save_image", "Save PNG image to file", {"save_image"});

  // Don't show the (overly verbose) message about the '--' flag
  parser.helpParams.showTerminator = false;

  try {
    parser.ParseCLI(argc, argv);
  } catch (const args::Completion &e) {
    std::cout << e.what();
    return 0;
  } catch (const args::Help &) {
    std::cout << parser;
    return 0;
  } catch (const args::ParseError &e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  } catch (const args::RequiredError &e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  }

  constexpr Scalar time_step = 1.0 / 240;
  constexpr int interval = 4;
  constexpr int horizon = 64;
  constexpr Scalar discount_factor = 0.99;
  // Use the provided random seed to generate all other seeds
  std::mt19937 generator(args::get(seed_flag));

  // Load rule graphs
  std::vector<Graph> rule_graphs = loadGraphs(args::get(graph_file_arg));
  if (rule_graphs.empty()) {
    std::cerr << "Graph file does not contain any graphs" << std::endl;
    return 1;
  }
  std::cout << "Number of graphs: " << rule_graphs.size() << std::endl;

  // Convert graphs to rules
  std::vector<Rule> rules;
  for (const Graph &rule_graph : rule_graphs) {
    rules.push_back(createRuleFromGraph(rule_graph));
  }

  // Generate a robot graph
  Graph robot_graph = {/*name=*/"robot",
                       /*nodes=*/{Node{"robot", NodeAttributes("robot")}},
                       /*edges=*/{},
                       /*subgraphs=*/{}};
  for (unsigned int rule_idx : args::get(rule_sequence_arg)) {
    if (rule_idx < rules.size()) {
      const Rule &rule = rules[rule_idx];
      std::vector<GraphMapping> matches = findMatches(rule.lhs_, robot_graph);
      if (!matches.empty()) {
        // Use the first match
        robot_graph = applyRule(rule, robot_graph, matches[0]);
      }
    }
  }

  auto robot = std::make_shared<Robot>(buildRobot(robot_graph));

  // Create a floor
  std::shared_ptr<Prop> floor = std::make_shared<Prop>(
      /*shape=*/PropShape::BOX,
      /*density=*/0.0, // static
      /*friction=*/0.9,
      /*half_extents=*/Vector3{10.0, 1.0, 10.0});

  // Find an initial y offset that will place the robot precisely on the ground
  Scalar y_offset;
  {
    BulletSimulation temp_sim(time_step);
    temp_sim.addRobot(robot, Vector3::Zero(), Quaternion::Identity());
    Vector3 lower, upper;
    temp_sim.getRobotWorldAABB(temp_sim.findRobotIndex(*robot), lower, upper);
    y_offset = -lower(1);
  }

  // Define a lambda function for making simulation instances
  auto make_sim_fn = [&]() -> std::shared_ptr<Simulation> {
    std::shared_ptr<BulletSimulation> sim =
        std::make_shared<BulletSimulation>(time_step);
    sim->addProp(floor, Vector3{0.0, -1.0, 0.0}, Quaternion::Identity());
    // Rotate 180 degrees around the y axis, so the base points to the right
    sim->addRobot(robot, Vector3{0.0, y_offset, 0.0},
                  Quaternion(Eigen::AngleAxis<Scalar>(M_PI, Vector3::UnitY())));
    return sim;
  };

  // Define an objective function
  SumOfSquaresObjective objective_fn;
  objective_fn.base_vel_ref_ = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0};
  objective_fn.base_vel_weight_ = Vector6::Ones();

  // Create the "main" simulation
  std::shared_ptr<Simulation> main_sim = make_sim_fn();
  Index robot_idx = main_sim->findRobotIndex(*robot);
  int dof_count = main_sim->getRobotDofCount(robot_idx);
  unsigned int thread_count = args::get(jobs_flag);
  if (thread_count == 0) {
    // Use the number of hardware threads available, which should be at least 1
    thread_count = std::max(std::thread::hardware_concurrency(), 1u);
  }
  auto value_estimator = std::make_shared<NullValueEstimator>();
  auto input_sampler = std::make_shared<DefaultInputSampler>();
  int episode_len = 250;
  int episode_count = args::get(episodes_flag);
  MatrixX input_sequence = MatrixX::Zero(dof_count, episode_len);
  MatrixX obs(value_estimator->getObservationSize(), episode_len + 1);
  VectorX rewards(episode_len);
  VectorX returns(episode_len + 1);
  MatrixX replay_obs(value_estimator->getObservationSize(), 0);
  VectorX replay_returns;

  if (args::get(optim_flag)) {
    for (int episode_idx = 0; episode_idx < episode_count; ++episode_idx) {
      std::cout << "Episode " << episode_idx << std::endl;

      unsigned int opt_seed = generator();
      MPPIOptimizer optimizer(
          /*kappa=*/100.0, /*discount_factor=*/discount_factor,
          /*dof_count=*/dof_count, /*interval=*/interval, /*horizon=*/horizon,
          /*sample_count=*/128, /*thread_count=*/thread_count,
          /*seed=*/opt_seed, /*make_sim_fn=*/make_sim_fn,
          /*objective_fn=*/objective_fn, /*value_estimator=*/value_estimator,
          /*input_sampler=*/input_sampler);
      for (int i = 0; i < 10; ++i) {
        optimizer.update();
      }

      // Run the main simulation in lockstep with the optimizer's simulations
      main_sim->saveState();
      for (int j = 0; j < input_sequence.cols(); ++j) {
        optimizer.update();
        input_sequence.col(j) = optimizer.input_sequence_.col(0);
        optimizer.advance(1);

        value_estimator->getObservation(*main_sim, obs.col(j));
        rewards(j) = 0.0;
        for (int i = 0; i < interval; ++i) {
          main_sim->setJointTargetPositions(robot_idx, input_sequence.col(j));
          main_sim->step();
          rewards(j) += objective_fn(*main_sim);
        }
      }
      value_estimator->getObservation(*main_sim, obs.col(episode_len));
      main_sim->restoreState();

      // Bootstrap returns with value estimator
      value_estimator->estimateValue(obs.col(episode_len), returns.tail<1>());
      for (int j = episode_len - 1; j >= 0; --j) {
        returns(j) = rewards(j) + discount_factor * returns(j + 1);
      }
      replay_obs.conservativeResize(Eigen::NoChange,
                                    replay_obs.cols() + episode_len);
      replay_obs.rightCols(episode_len) = obs.leftCols(episode_len);
      replay_returns.conservativeResize(replay_returns.size() + episode_len);
      replay_returns.tail(episode_len) = returns.head(episode_len);
      value_estimator->train(replay_obs, replay_returns);

      std::cout << "Total reward: " << rewards.sum() << std::endl;
    }
  }

  main_sim->saveState();

  std::string save_image_path = args::get(save_image_flag);
  if (!save_image_path.empty()) {
    GLFWViewer viewer(/*hidden=*/false);
    viewer.update(time_step);
    int fb_width, fb_height;
    viewer.getFramebufferSize(fb_width, fb_height);
    std::unique_ptr<unsigned char[]> rgba(
        new unsigned char[4 * fb_width * fb_height]);
    viewer.render(*main_sim, rgba.get());
    std::unique_ptr<unsigned char[]> rgba_flipped(
        new unsigned char[4 * fb_width * fb_height]);
    for (int i = 0; i < fb_height; ++i) {
      std::memcpy(&rgba_flipped[i * fb_width * 4],
                  &rgba[(fb_height - i - 1) * fb_width * 4], fb_width * 4);
    }
    unsigned int error = lodepng::encode(save_image_path, rgba_flipped.get(),
                                         fb_width, fb_height);
    if (error) {
      std::cerr << "Failed to save image: " << lodepng_error_text(error)
                << std::endl;
    }
  }

  if (args::get(render_flag)) {
    // View the trajectory
    GLFWViewer viewer;
    double sim_time = glfwGetTime();
    int i = 0, j = 0;
    while (!viewer.shouldClose()) {
      double current_time = glfwGetTime();
      while (sim_time < current_time) {
        main_sim->setJointTargetPositions(robot_idx, input_sequence.col(j));
        main_sim->step();
        viewer.update(time_step);
        sim_time += time_step;
        ++i;
        if (i >= interval) {
          i = 0;
          ++j;
        }
        if (j >= input_sequence.cols()) {
          i = 0;
          j = 0;
          main_sim->restoreState();
        }
      }
      viewer.render(*main_sim);
    }
  }
}
