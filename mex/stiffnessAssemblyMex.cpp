#include "mex.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

using Clock = std::chrono::high_resolution_clock;

int g_threadOverride = 0;

struct Triplet {
    std::uint64_t key;
    double value;
};

struct BuildProfile {
    double elementSeconds = 0.0;
    double sortSeconds = 0.0;
    double compressSeconds = 0.0;
    double sparseFillSeconds = 0.0;
    double rhsSeconds = 0.0;
    double totalSeconds = 0.0;
    int actualThreads = 1;
    mwSize rawTriplets = 0;
    mwSize uniqueNonzeros = 0;
};

double elapsedSeconds(const Clock::time_point& t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

int openmpMaxThreads()
{
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

int openmpNumProcs()
{
#ifdef _OPENMP
    return omp_get_num_procs();
#else
    unsigned int n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : static_cast<int>(n);
#endif
}

int hardwareConcurrency()
{
    unsigned int n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : static_cast<int>(n);
}

mxArray* makeThreadStatus()
{
    const char* names[] = {
        "openmp", "threadOverride", "ompMaxThreads", "ompNumProcs",
        "hardwareConcurrency"
    };
    mxArray* s = mxCreateStructMatrix(1, 1, 5, names);
#ifdef _OPENMP
    mxSetField(s, 0, "openmp", mxCreateLogicalScalar(true));
#else
    mxSetField(s, 0, "openmp", mxCreateLogicalScalar(false));
#endif
    mxSetField(s, 0, "threadOverride", mxCreateDoubleScalar(g_threadOverride));
    mxSetField(s, 0, "ompMaxThreads", mxCreateDoubleScalar(openmpMaxThreads()));
    mxSetField(s, 0, "ompNumProcs", mxCreateDoubleScalar(openmpNumProcs()));
    mxSetField(s, 0, "hardwareConcurrency", mxCreateDoubleScalar(hardwareConcurrency()));
    return s;
}

#ifdef _WIN32
struct CpuSetRecord {
    DWORD id;
    WORD group;
    BYTE logicalProcessorIndex;
    BYTE efficiencyClass;
    bool parked;
};

std::vector<CpuSetRecord> queryWindowsCpuSets()
{
    DWORD bytesNeeded = 0;
    GetSystemCpuSetInformation(nullptr, 0, &bytesNeeded, nullptr, 0);
    if (bytesNeeded == 0) {
        mexErrMsgIdAndTxt("cpuBench:cpuSetsUnavailable",
            "Windows CPU Set information is unavailable on this system.");
    }

    std::vector<char> buffer(bytesNeeded);
    if (!GetSystemCpuSetInformation(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
            bytesNeeded, &bytesNeeded, nullptr, 0)) {
        mexErrMsgIdAndTxt("cpuBench:cpuSetsUnavailable",
            "GetSystemCpuSetInformation failed.");
    }

    std::vector<CpuSetRecord> records;
    DWORD offset = 0;
    while (offset < bytesNeeded) {
        PSYSTEM_CPU_SET_INFORMATION info =
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + offset);
        if (info->Type == CpuSetInformation) {
            CpuSetRecord rec;
            rec.id = info->CpuSet.Id;
            rec.group = info->CpuSet.Group;
            rec.logicalProcessorIndex = info->CpuSet.LogicalProcessorIndex;
            rec.efficiencyClass = info->CpuSet.EfficiencyClass;
            rec.parked = info->CpuSet.Parked != 0;
            records.push_back(rec);
        }
        if (info->Size == 0) {
            break;
        }
        offset += info->Size;
    }

    if (records.empty()) {
        mexErrMsgIdAndTxt("cpuBench:cpuSetsUnavailable",
            "No Windows CPU Set records were returned.");
    }
    return records;
}

bool restoreProcessCpuSelection()
{
    HANDLE process = GetCurrentProcess();
    SetProcessDefaultCpuSets(process, nullptr, 0);

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(process, &processMask, &systemMask)) {
        return false;
    }
    if (systemMask == 0) {
        return false;
    }
    return SetProcessAffinityMask(process, systemMask) != 0;
}

mxArray* makeDoubleVector(const std::vector<double>& values)
{
    mxArray* out = mxCreateDoubleMatrix(1, values.size(), mxREAL);
    double* data = mxGetPr(out);
    for (mwSize i = 0; i < values.size(); ++i) {
        data[i] = values[i];
    }
    return out;
}

void splitCpuSets(const std::vector<CpuSetRecord>& records,
    std::vector<DWORD>& allIds, std::vector<DWORD>& pIds,
    std::vector<double>& logicalProcessors, std::vector<double>& pLogicalProcessors,
    std::vector<double>& efficiencyClasses, std::vector<double>& parkedFlags,
    std::vector<double>& pParkedFlags, int& minEfficiency, int& maxEfficiency)
{
    minEfficiency = 255;
    maxEfficiency = 0;
    for (const CpuSetRecord& rec : records) {
        const int eff = static_cast<int>(rec.efficiencyClass);
        minEfficiency = std::min(minEfficiency, eff);
        maxEfficiency = std::max(maxEfficiency, eff);
    }

    for (const CpuSetRecord& rec : records) {
        allIds.push_back(rec.id);
        logicalProcessors.push_back(static_cast<double>(rec.logicalProcessorIndex));
        efficiencyClasses.push_back(static_cast<double>(rec.efficiencyClass));
        parkedFlags.push_back(rec.parked ? 1.0 : 0.0);
        if (static_cast<int>(rec.efficiencyClass) == maxEfficiency) {
            pIds.push_back(rec.id);
            pLogicalProcessors.push_back(static_cast<double>(rec.logicalProcessorIndex));
            pParkedFlags.push_back(rec.parked ? 1.0 : 0.0);
        }
    }
}

DWORD_PTR logicalProcessorMask(const std::vector<double>& logicalProcessors)
{
    DWORD_PTR mask = 0;
    for (double value : logicalProcessors) {
        const int cpu = static_cast<int>(value);
        if (cpu < 0 || cpu >= static_cast<int>(sizeof(DWORD_PTR) * 8)) {
            mexErrMsgIdAndTxt("cpuBench:cpuGroupUnsupported",
                "CPU affinity mask only supports the first processor group.");
        }
        mask |= (static_cast<DWORD_PTR>(1) << cpu);
    }
    return mask;
}

mxArray* setWindowsCpuMode(const std::string& mode)
{
    const bool restoreBeforeQuery = restoreProcessCpuSelection();
    std::vector<CpuSetRecord> records = queryWindowsCpuSets();
    std::vector<DWORD> allIds;
    std::vector<DWORD> pIds;
    std::vector<double> logicalProcessors;
    std::vector<double> pLogicalProcessors;
    std::vector<double> efficiencyClasses;
    std::vector<double> parkedFlags;
    std::vector<double> pParkedFlags;
    int minEfficiency = 0;
    int maxEfficiency = 0;
    splitCpuSets(records, allIds, pIds, logicalProcessors, pLogicalProcessors,
        efficiencyClasses, parkedFlags, pParkedFlags, minEfficiency, maxEfficiency);

    if (allIds.empty()) {
        mexErrMsgIdAndTxt("cpuBench:noCpuSets", "No CPU sets were found.");
    }

    std::vector<DWORD>* selectedIds = &allIds;
    std::vector<double>* selectedLogical = &logicalProcessors;
    std::string selectedMode = "all";
    std::string message = "Using all logical processors, including currently parked processors.";

    if (mode == "p" || mode == "pcore" || mode == "pcores") {
        if (maxEfficiency == minEfficiency || pIds.empty()) {
            mexErrMsgIdAndTxt("cpuBench:noHybridCpu",
                "P-core mode requires a Windows hybrid CPU with multiple EfficiencyClass values.");
        }
        selectedIds = &pIds;
        selectedLogical = &pLogicalProcessors;
        selectedMode = "pcores";
        message = "Using logical processors with the highest Windows EfficiencyClass, including currently parked processors.";
    } else if (!(mode == "all" || mode == "pe" || mode == "amd")) {
        mexErrMsgIdAndTxt("cpuBench:invalidCpuMode",
            "CPU mode must be 'pcores', 'all', 'pe', or 'amd'.");
    }

    HANDLE process = GetCurrentProcess();
    if (selectedMode == "all") {
        if (!restoreProcessCpuSelection()) {
            mexErrMsgIdAndTxt("cpuBench:restoreAffinityFailed",
                "Could not restore the MATLAB process to all system processors.");
        }
    } else {
        const DWORD_PTR mask = logicalProcessorMask(*selectedLogical);
        if (!SetProcessAffinityMask(process, mask)) {
            mexErrMsgIdAndTxt("cpuBench:setAffinityFailed",
                "SetProcessAffinityMask failed for P-core mode.");
        }
        if (!SetProcessDefaultCpuSets(process, selectedIds->data(),
                static_cast<ULONG>(selectedIds->size()))) {
            mexErrMsgIdAndTxt("cpuBench:setCpuSetsFailed",
                "SetProcessDefaultCpuSets failed for P-core mode.");
        }
    }

    const char* names[] = {
        "supported", "mode", "message", "minEfficiencyClass", "maxEfficiencyClass",
        "logicalProcessors", "efficiencyClasses", "parkedFlags",
        "pLogicalProcessors", "pParkedFlags", "selectedLogicalProcessors",
        "selectedLogicalCount", "recommendedThreads", "restoreBeforeQuery"
    };
    mxArray* s = mxCreateStructMatrix(1, 1, 14, names);
    mxSetField(s, 0, "supported", mxCreateLogicalScalar(true));
    mxSetField(s, 0, "mode", mxCreateString(selectedMode.c_str()));
    mxSetField(s, 0, "message", mxCreateString(message.c_str()));
    mxSetField(s, 0, "minEfficiencyClass", mxCreateDoubleScalar(minEfficiency));
    mxSetField(s, 0, "maxEfficiencyClass", mxCreateDoubleScalar(maxEfficiency));
    mxSetField(s, 0, "logicalProcessors", makeDoubleVector(logicalProcessors));
    mxSetField(s, 0, "efficiencyClasses", makeDoubleVector(efficiencyClasses));
    mxSetField(s, 0, "parkedFlags", makeDoubleVector(parkedFlags));
    mxSetField(s, 0, "pLogicalProcessors", makeDoubleVector(pLogicalProcessors));
    mxSetField(s, 0, "pParkedFlags", makeDoubleVector(pParkedFlags));
    mxSetField(s, 0, "selectedLogicalProcessors", makeDoubleVector(*selectedLogical));
    mxSetField(s, 0, "selectedLogicalCount",
        mxCreateDoubleScalar(static_cast<double>(selectedLogical->size())));
    mxSetField(s, 0, "recommendedThreads",
        mxCreateDoubleScalar(static_cast<double>(selectedLogical->size())));
    mxSetField(s, 0, "restoreBeforeQuery", mxCreateLogicalScalar(restoreBeforeQuery));
    return s;
}
#else
mxArray* setWindowsCpuMode(const std::string& mode)
{
    if (mode == "p" || mode == "pcore" || mode == "pcores") {
        mexErrMsgIdAndTxt("cpuBench:cpuModeUnsupported",
            "P-core mode is currently implemented for Windows CPU Sets only.");
    }

    const int threads = hardwareConcurrency();
    const char* names[] = {
        "supported", "mode", "message", "selectedLogicalCount", "recommendedThreads"
    };
    mxArray* s = mxCreateStructMatrix(1, 1, 5, names);
    mxSetField(s, 0, "supported", mxCreateLogicalScalar(false));
    mxSetField(s, 0, "mode", mxCreateString("all"));
    mxSetField(s, 0, "message",
        mxCreateString("CPU mode affinity is not implemented on this platform; using all cores."));
    mxSetField(s, 0, "selectedLogicalCount", mxCreateDoubleScalar(threads));
    mxSetField(s, 0, "recommendedThreads", mxCreateDoubleScalar(threads));
    return s;
}
#endif

int scalarToInt(const mxArray* value, const char* name)
{
    if (!mxIsDouble(value) || mxIsComplex(value) || mxGetNumberOfElements(value) != 1) {
        mexErrMsgIdAndTxt("cpuBench:invalidInput", "%s must be a real scalar double.", name);
    }
    double d = mxGetScalar(value);
    if (!std::isfinite(d) || d < 1.0 || std::floor(d) != d ||
        d > static_cast<double>(std::numeric_limits<int>::max())) {
        mexErrMsgIdAndTxt("cpuBench:invalidInput", "%s must be a positive integer.", name);
    }
    return static_cast<int>(d);
}

std::string mxString(const mxArray* value)
{
    if (!mxIsChar(value)) {
        mexErrMsgIdAndTxt("cpuBench:invalidCommand", "Command must be a character vector or string scalar.");
    }
    char* c = mxArrayToString(value);
    if (c == nullptr) {
        mexErrMsgIdAndTxt("cpuBench:invalidCommand", "Could not read command string.");
    }
    std::string out(c);
    mxFree(c);
    return out;
}

mwSize nodeIndex(int i, int j, int k, int nx, int ny)
{
    return static_cast<mwSize>(i) +
        static_cast<mwSize>(nx + 1) *
        (static_cast<mwSize>(j) + static_cast<mwSize>(ny + 1) * static_cast<mwSize>(k));
}

void computeHexStiffness(double* ke, double hx, double hy, double hz, double young, double poisson)
{
    for (int i = 0; i < 24 * 24; ++i) {
        ke[i] = 0.0;
    }

    const double lambda = young * poisson / ((1.0 + poisson) * (1.0 - 2.0 * poisson));
    const double mu = young / (2.0 * (1.0 + poisson));
    const double gp = 0.57735026918962576451;
    const double gauss[2] = {-gp, gp};
    const int sx[8] = {-1,  1,  1, -1, -1,  1,  1, -1};
    const int sy[8] = {-1, -1,  1,  1, -1, -1,  1,  1};
    const int sz[8] = {-1, -1, -1, -1,  1,  1,  1,  1};
    const double detJ = hx * hy * hz / 8.0;

    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                const double xi = gauss[ix];
                const double eta = gauss[iy];
                const double zeta = gauss[iz];
                double grad[8][3];
                for (int a = 0; a < 8; ++a) {
                    const double dNdxi = 0.125 * sx[a] * (1.0 + sy[a] * eta) * (1.0 + sz[a] * zeta);
                    const double dNdeta = 0.125 * sy[a] * (1.0 + sx[a] * xi) * (1.0 + sz[a] * zeta);
                    const double dNdzeta = 0.125 * sz[a] * (1.0 + sx[a] * xi) * (1.0 + sy[a] * eta);
                    grad[a][0] = dNdxi * 2.0 / hx;
                    grad[a][1] = dNdeta * 2.0 / hy;
                    grad[a][2] = dNdzeta * 2.0 / hz;
                }

                for (int a = 0; a < 8; ++a) {
                    for (int b = 0; b < 8; ++b) {
                        const double dot = grad[a][0] * grad[b][0] +
                            grad[a][1] * grad[b][1] + grad[a][2] * grad[b][2];
                        for (int compA = 0; compA < 3; ++compA) {
                            for (int compB = 0; compB < 3; ++compB) {
                                const double delta = (compA == compB) ? 1.0 : 0.0;
                                const double value =
                                    lambda * grad[a][compA] * grad[b][compB] +
                                    mu * delta * dot +
                                    mu * grad[a][compB] * grad[b][compA];
                                const int row = 3 * a + compA;
                                const int col = 3 * b + compB;
                                ke[row + 24 * col] += value * detJ;
                            }
                        }
                    }
                }
            }
        }
    }

    const double diagShift = young * hx * hy * hz * 1.0e-10;
    for (int d = 0; d < 24; ++d) {
        ke[d + 24 * d] += diagShift;
    }
}

void fillRhs(double* rhs, int nx, int ny, int nz)
{
    const mwSize numNodes = static_cast<mwSize>(nx + 1) * static_cast<mwSize>(ny + 1) *
        static_cast<mwSize>(nz + 1);
    const mwSize ndof = 3 * numNodes;
    for (mwSize i = 0; i < ndof; ++i) {
        rhs[i] = 0.0;
    }

    const double faceScale = 1.0 / static_cast<double>((ny + 1) * (nz + 1));
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            const mwSize n = nodeIndex(nx, j, k, nx, ny);
            const double y = (ny == 0) ? 0.0 : static_cast<double>(j) / static_cast<double>(ny);
            const double z = (nz == 0) ? 0.0 : static_cast<double>(k) / static_cast<double>(nz);
            rhs[3 * n + 0] += 0.04 * faceScale * std::sin(3.14159265358979323846 * z);
            rhs[3 * n + 1] += 0.02 * faceScale * std::cos(3.14159265358979323846 * y);
            rhs[3 * n + 2] -= faceScale;
        }
    }
}

mxArray* makeProfileStruct(const BuildProfile& p, int nx, int ny, int nz, mwSize ndof)
{
    const char* profileNames[] = {
        "nx", "ny", "nz", "elements", "dofs", "openmp", "threadOverride",
        "ompMaxThreads", "ompNumProcs", "hardwareConcurrency", "actualThreads",
        "rawTriplets", "uniqueNonzeros", "elementSeconds", "sortSeconds",
        "compressSeconds", "sparseFillSeconds", "rhsSeconds", "totalSeconds",
        "stiffnessSeconds"
    };
    mxArray* s = mxCreateStructMatrix(1, 1, 20, profileNames);
    const mwSize elements = static_cast<mwSize>(nx) * static_cast<mwSize>(ny) * static_cast<mwSize>(nz);

    mxSetField(s, 0, "nx", mxCreateDoubleScalar(nx));
    mxSetField(s, 0, "ny", mxCreateDoubleScalar(ny));
    mxSetField(s, 0, "nz", mxCreateDoubleScalar(nz));
    mxSetField(s, 0, "elements", mxCreateDoubleScalar(static_cast<double>(elements)));
    mxSetField(s, 0, "dofs", mxCreateDoubleScalar(static_cast<double>(ndof)));
#ifdef _OPENMP
    mxSetField(s, 0, "openmp", mxCreateLogicalScalar(true));
#else
    mxSetField(s, 0, "openmp", mxCreateLogicalScalar(false));
#endif
    mxSetField(s, 0, "threadOverride", mxCreateDoubleScalar(g_threadOverride));
    mxSetField(s, 0, "ompMaxThreads", mxCreateDoubleScalar(openmpMaxThreads()));
    mxSetField(s, 0, "ompNumProcs", mxCreateDoubleScalar(openmpNumProcs()));
    mxSetField(s, 0, "hardwareConcurrency", mxCreateDoubleScalar(hardwareConcurrency()));
    mxSetField(s, 0, "actualThreads", mxCreateDoubleScalar(p.actualThreads));
    mxSetField(s, 0, "rawTriplets", mxCreateDoubleScalar(static_cast<double>(p.rawTriplets)));
    mxSetField(s, 0, "uniqueNonzeros", mxCreateDoubleScalar(static_cast<double>(p.uniqueNonzeros)));
    mxSetField(s, 0, "elementSeconds", mxCreateDoubleScalar(p.elementSeconds));
    mxSetField(s, 0, "sortSeconds", mxCreateDoubleScalar(p.sortSeconds));
    mxSetField(s, 0, "compressSeconds", mxCreateDoubleScalar(p.compressSeconds));
    mxSetField(s, 0, "sparseFillSeconds", mxCreateDoubleScalar(p.sparseFillSeconds));
    mxSetField(s, 0, "rhsSeconds", mxCreateDoubleScalar(p.rhsSeconds));
    mxSetField(s, 0, "totalSeconds", mxCreateDoubleScalar(p.totalSeconds));
    mxSetField(s, 0, "stiffnessSeconds", mxCreateDoubleScalar(
        p.elementSeconds + p.sortSeconds + p.compressSeconds + p.sparseFillSeconds));
    return s;
}

void buildSystem(int nlhs, mxArray* plhs[], int nx, int ny, int nz)
{
    const Clock::time_point totalStart = Clock::now();
    const mwSize numElements = static_cast<mwSize>(nx) * static_cast<mwSize>(ny) * static_cast<mwSize>(nz);
    const mwSize numNodes = static_cast<mwSize>(nx + 1) * static_cast<mwSize>(ny + 1) *
        static_cast<mwSize>(nz + 1);
    const mwSize ndof = 3 * numNodes;
    const mwSize entriesPerElement = 24 * 24;
    const mwSize rawTriplets = numElements * entriesPerElement;

    if (numElements == 0 || ndof == 0) {
        mexErrMsgIdAndTxt("cpuBench:invalidSize", "Problem dimensions must produce at least one element.");
    }
    if (rawTriplets / entriesPerElement != numElements) {
        mexErrMsgIdAndTxt("cpuBench:sizeOverflow", "Problem is too large for this benchmark triplet layout.");
    }

#ifdef _OPENMP
    if (g_threadOverride > 0) {
        omp_set_num_threads(g_threadOverride);
    }
#endif

    BuildProfile profile;
    profile.rawTriplets = rawTriplets;
    std::vector<Triplet> triplets(rawTriplets);

    const double hx = 1.0 / static_cast<double>(nx);
    const double hy = 1.0 / static_cast<double>(ny);
    const double hz = 1.0 / static_cast<double>(nz);
    const double poisson = 0.30;
    const double baseYoung = 210.0;

    const Clock::time_point elementStart = Clock::now();

#ifdef _OPENMP
#pragma omp parallel
    {
#pragma omp single
        {
            profile.actualThreads = omp_get_num_threads();
        }
#pragma omp for schedule(static)
        for (long long linear = 0; linear < static_cast<long long>(numElements); ++linear) {
#else
    for (long long linear = 0; linear < static_cast<long long>(numElements); ++linear) {
#endif
            const int ex = static_cast<int>(linear % nx);
            const int ey = static_cast<int>((linear / nx) % ny);
            const int ez = static_cast<int>(linear / (static_cast<long long>(nx) * ny));
            const double young = baseYoung * (1.0 + 0.03 *
                std::sin(0.37 * static_cast<double>(ex + 1) +
                    0.23 * static_cast<double>(ey + 1) +
                    0.19 * static_cast<double>(ez + 1)));

            double ke[24 * 24];
            computeHexStiffness(ke, hx, hy, hz, young, poisson);

            const mwSize elemNodes[8] = {
                nodeIndex(ex,     ey,     ez,     nx, ny),
                nodeIndex(ex + 1, ey,     ez,     nx, ny),
                nodeIndex(ex + 1, ey + 1, ez,     nx, ny),
                nodeIndex(ex,     ey + 1, ez,     nx, ny),
                nodeIndex(ex,     ey,     ez + 1, nx, ny),
                nodeIndex(ex + 1, ey,     ez + 1, nx, ny),
                nodeIndex(ex + 1, ey + 1, ez + 1, nx, ny),
                nodeIndex(ex,     ey + 1, ez + 1, nx, ny)
            };

            mwSize elemDofs[24];
            for (int a = 0; a < 8; ++a) {
                elemDofs[3 * a + 0] = 3 * elemNodes[a] + 0;
                elemDofs[3 * a + 1] = 3 * elemNodes[a] + 1;
                elemDofs[3 * a + 2] = 3 * elemNodes[a] + 2;
            }

            const mwSize offset = static_cast<mwSize>(linear) * entriesPerElement;
            for (int colLocal = 0; colLocal < 24; ++colLocal) {
                const mwSize col = elemDofs[colLocal];
                for (int rowLocal = 0; rowLocal < 24; ++rowLocal) {
                    const mwSize row = elemDofs[rowLocal];
                    const mwSize slot = offset + static_cast<mwSize>(colLocal) * 24 +
                        static_cast<mwSize>(rowLocal);
                    triplets[slot].key = static_cast<std::uint64_t>(col) *
                        static_cast<std::uint64_t>(ndof) + static_cast<std::uint64_t>(row);
                    triplets[slot].value = ke[rowLocal + 24 * colLocal];
                }
            }
#ifdef _OPENMP
        }
    }
#else
    }
#endif

    profile.elementSeconds = elapsedSeconds(elementStart);

    const Clock::time_point sortStart = Clock::now();
    std::sort(triplets.begin(), triplets.end(), [](const Triplet& a, const Triplet& b) {
        return a.key < b.key;
    });
    profile.sortSeconds = elapsedSeconds(sortStart);

    const Clock::time_point compressStart = Clock::now();
    const double dropTol = 0.0;
    mwSize write = 0;
    mwSize read = 0;
    while (read < rawTriplets) {
        const std::uint64_t key = triplets[read].key;
        double value = 0.0;
        do {
            value += triplets[read].value;
            ++read;
        } while (read < rawTriplets && triplets[read].key == key);

        if (std::abs(value) > dropTol) {
            triplets[write].key = key;
            triplets[write].value = value;
            ++write;
        }
    }
    triplets.resize(write);
    profile.uniqueNonzeros = write;
    profile.compressSeconds = elapsedSeconds(compressStart);

    const Clock::time_point sparseStart = Clock::now();
    mxArray* K = mxCreateSparse(ndof, ndof, write, mxREAL);
    mwIndex* ir = mxGetIr(K);
    mwIndex* jc = mxGetJc(K);
    double* pr = mxGetPr(K);

    mwIndex currentCol = 0;
    jc[0] = 0;
    for (mwSize idx = 0; idx < write; ++idx) {
        const std::uint64_t key = triplets[idx].key;
        const mwIndex col = static_cast<mwIndex>(key / static_cast<std::uint64_t>(ndof));
        const mwIndex row = static_cast<mwIndex>(key % static_cast<std::uint64_t>(ndof));
        while (currentCol < col) {
            ++currentCol;
            jc[currentCol] = static_cast<mwIndex>(idx);
        }
        ir[idx] = row;
        pr[idx] = triplets[idx].value;
    }
    while (currentCol < static_cast<mwIndex>(ndof)) {
        ++currentCol;
        jc[currentCol] = static_cast<mwIndex>(write);
    }
    profile.sparseFillSeconds = elapsedSeconds(sparseStart);

    mxArray* rhs = mxCreateDoubleMatrix(ndof, 1, mxREAL);
    const Clock::time_point rhsStart = Clock::now();
    fillRhs(mxGetPr(rhs), nx, ny, nz);
    profile.rhsSeconds = elapsedSeconds(rhsStart);
    profile.totalSeconds = elapsedSeconds(totalStart);

    plhs[0] = K;
    if (nlhs > 1) {
        plhs[1] = rhs;
    } else {
        mxDestroyArray(rhs);
    }
    if (nlhs > 2) {
        plhs[2] = makeProfileStruct(profile, nx, ny, nz, ndof);
    }
}

}  // namespace

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[])
{
    if (nrhs == 0) {
        mexErrMsgIdAndTxt("cpuBench:notEnoughInputs",
            "Usage: stiffnessAssemblyMex(nx, ny, nz) or stiffnessAssemblyMex('command', ...).");
    }

    if (mxIsChar(prhs[0])) {
        const std::string command = mxString(prhs[0]);
        if (command == "setThreads") {
            if (nrhs != 2) {
                mexErrMsgIdAndTxt("cpuBench:setThreads", "setThreads requires one positive integer.");
            }
            g_threadOverride = scalarToInt(prhs[1], "thread count");
#ifdef _OPENMP
            omp_set_num_threads(g_threadOverride);
#endif
            if (nlhs > 0) {
                plhs[0] = makeThreadStatus();
            }
            return;
        }
        if (command == "resetThreads") {
            g_threadOverride = 0;
            if (nlhs > 0) {
                plhs[0] = makeThreadStatus();
            }
            return;
        }
        if (command == "getThreads") {
            if (nlhs > 0) {
                plhs[0] = makeThreadStatus();
            }
            return;
        }
        if (command == "setCpuMode") {
            if (nrhs != 2) {
                mexErrMsgIdAndTxt("cpuBench:setCpuMode",
                    "setCpuMode requires one mode: 'pcores', 'pe', 'amd', or 'all'.");
            }
            if (nlhs > 0) {
                plhs[0] = setWindowsCpuMode(mxString(prhs[1]));
            } else {
                mxDestroyArray(setWindowsCpuMode(mxString(prhs[1])));
            }
            return;
        }
        mexErrMsgIdAndTxt("cpuBench:unknownCommand", "Unknown command: %s", command.c_str());
    }

    if (nrhs < 3) {
        mexErrMsgIdAndTxt("cpuBench:notEnoughInputs", "nx, ny, and nz are required.");
    }
    if (nlhs > 3) {
        mexErrMsgIdAndTxt("cpuBench:tooManyOutputs", "Use at most three outputs: K, rhs, profile.");
    }

    const int nx = scalarToInt(prhs[0], "nx");
    const int ny = scalarToInt(prhs[1], "ny");
    const int nz = scalarToInt(prhs[2], "nz");

    buildSystem(nlhs, plhs, nx, ny, nz);
}
