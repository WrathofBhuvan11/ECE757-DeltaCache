#include <iostream>
#include <vector>

int main() {
    const int N = 100000;
    const double a = 2.5;

    std::vector<double> x(N);
    std::vector<double> y(N);

    for (int i = 0; i < N; i++) {
        x[i] = i * 1.0;
        y[i] = i * 0.5;
    }

    // DAXPY: y = a*x + y
    for (int i = 0; i < N; i++) {
        y[i] = a * x[i] + y[i];
    }

    std::cout << "y[9999] = " << y[9999] << "\n";
    return 0;
}
