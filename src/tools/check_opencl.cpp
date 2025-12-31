#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>

int main() {
  std::cout << "=== OpenCV Build Information ===" << std::endl;
  std::cout << cv::getBuildInformation() << std::endl;

  std::cout << "\n=== OpenCL Status ===" << std::endl;
  if (cv::ocl::haveOpenCL()) {
    std::cout << "OpenCL Available: YES" << std::endl;
    cv::ocl::setUseOpenCL(true);
    cv::ocl::Device dev = cv::ocl::Device::getDefault();
    std::cout << "Device Name: " << dev.name() << std::endl;
    std::cout << "Device Vendor: " << dev.vendorName() << std::endl;
    std::cout << "Driver Version: " << dev.driverVersion() << std::endl;
    std::cout << "OpenCV Default Device: " << dev.name() << std::endl;
  } else {
    std::cout << "OpenCL Available: NO" << std::endl;
    std::cout << "Check if libopencv-dev was built with WITH_OPENCL=ON"
              << std::endl;
    std::cout
        << "Check if OpenCL drivers (rocm-opencl / mesa-opencl) are installed."
        << std::endl;
  }
  return 0;
}
