//stride struct

struct stride{
    int stride;
    int pass;
    int share;
    struct proc* proc;
};

struct FQ{
    int total;
    int timeallot;
    int current;
    struct proc* holdingproc[NPROC];
};

