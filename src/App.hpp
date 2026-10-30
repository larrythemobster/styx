#pragma once

namespace Styx {
class App {
public:
    App(int &argc, char **argv);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;
    int exec();
    static void quit(int code = 0);
private:
    static bool stopping;
    static int exitCode;
};
}
