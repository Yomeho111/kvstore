#include "replicate.h"

int main()
{
    replicate::MasterServer master("10.0.0.4", 20000);

    master.init();
    master.send(".tmp");

    return 0;
}