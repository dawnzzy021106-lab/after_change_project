#include "coordinator.h"

using namespace ECProject;
int main(int argc, char **argv)
{
  if (false) {
    umask(0);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
  }
  
  char buff[256];
  getcwd(buff, 256); // 获取当前工作目录，存入buff数组
  std::string cwf = std::string(argv[0]);
  std::string xml_path = std::string(buff) +
      cwf.substr(1, cwf.rfind('/') - 1) + "/../clusterinfo.xml";
  // 创建 coordinator 实例
  std::cout << "DEBUG: 正在尝试创建 Coordinator 对象..." << std::endl;
  Coordinator coordinator("0.0.0.0", COORDINATOR_PORT, xml_path);
  // 如果运行命令有参数，则将该参数作为ec配置文件路径
  if (argc == 2) {
    std::string config_file = std::string(buff) +
        cwf.substr(1, cwf.rfind('/') - 1) + "/../" + std::string(argv[1]);
    std::cout << "DEBUG: 正在初始化 EC Schema..." << std::endl;
    coordinator.init_ec_schema(config_file);
  }
  std::cout << "DEBUG: 正在进入 coordinator.run()..." << std::endl;
  coordinator.run();
  return 0;
}