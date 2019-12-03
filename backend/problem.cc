#include <iostream>
#include <fstream>
#include <eigen3/Eigen/Dense>
// #include <glog/logging.h>      //google的日志系统http://senlinzhan.github.io/2017/10/07/glog/
#include "backend/problem.h"
#include "utils/tic_toc.h"

#ifdef USE_OPENMP

#include <omp.h>

#endif

using namespace std;


namespace myslam {
namespace backend {
void Problem::LogoutVectorSize() {
    // LOG(INFO) <<
    //           "1 problem::LogoutVectorSize verticies_:" << verticies_.size() <<
    //           " edges:" << edges_.size();
    std::cout<<
              "1 problem::LogoutVectorSize verticies_:" << verticies_.size() <<
              " edges:" << edges_.size();

}

Problem::Problem(ProblemType problemType) :
        problemType_(problemType) {
    LogoutVectorSize();
    verticies_marg_.clear();
}

Problem::~Problem() {}

bool Problem::AddVertex(std::shared_ptr<Vertex> vertex) {
    if (verticies_.find(vertex->Id()) != verticies_.end()) {
        // LOG(WARNING) << "Vertex " << vertex->Id() << " has been added before";
        return false;
    } else {
        //https://blog.csdn.net/cbNotes/article/details/76594435 插入数据的四中方法
        verticies_.insert(pair<ulong, shared_ptr<Vertex>>(vertex->Id(), vertex));
    }

    return true;
}



bool Problem::AddEdge(shared_ptr<Edge> edge) {
    if (edges_.find(edge->Id()) == edges_.end()) {
        edges_.insert(pair<ulong, std::shared_ptr<Edge>>(edge->Id(), edge));
    } else {
        // LOG(WARNING) << "Edge " << edge->Id() << " has been added before!";
        return false;
    }

    for (auto &vertex: edge->Verticies()) {
        vertexToEdge_.insert(pair<ulong, shared_ptr<Edge>>(vertex->Id(), edge));
    }
    return true;
}


bool Problem::Solve(int iterations) {


    if (edges_.size() == 0 || verticies_.size() == 0) {
        std::cerr << "\nCannot solve problem without edges or verticies" << std::endl;
        return false;
    }

    TicToc t_solve;
    // 统计优化变量的维数，为构建 H 矩阵做准备
    SetOrdering();
    // 遍历edge, 构建 H = J^T * J 矩阵以及法方程
    MakeHessian();
    // LM 初始化,设定初始的lambda
    ComputeLambdaInitLM();
    // LM 算法迭代求解
    bool stop = false;
    int iter = 0;
    while (!stop && (iter < iterations)) {
        std::cout << "iter: " << iter << " , chi= " << currentChi_ << " , Lambda= " << currentLambda_<< std::endl;
        bool oneStepSuccess = false;
        int false_cnt = 0;
        while (!oneStepSuccess)  // 不断尝试 Lambda, 直到成功迭代一步
        {
            //lambda加到矩阵上 
            AddLambdatoHessianLM();
            // 第四步，解线性方程 H X = B
            SolveLinearSystem();
            //lambda从矩阵中减去，玩那?不是在玩，是在试探lambda多大合适
            RemoveLambdaHessianLM();

            // 优化退出条件1： delta_x_ 很小则退出
            if (delta_x_.squaredNorm() <= 1e-6) {
                std::cout<<GREEN;
                std::cout<<"Enough precision!(1e-6)"<<std::endl;
                std::cout<<RESET;
                stop = true;
                break;
            }
            if (false_cnt > 10) {
                std::cout<<RED;
                std::cout <<"\n Can't find appropriate step, please resetinitial values of vertices.\n"<< std::endl;
                std::cout<<RESET;
                stop = true;
                break;
            }
            
            // 更新状态量 X = X+ delta_x
            UpdateStates();
            // 判断当前步是否可行以及 LM 的 lambda 怎么更新
            oneStepSuccess = IsGoodStepInLM();
            // 后续处理，
            if (oneStepSuccess) {
                // 在新线性化点 构建 hessian
                MakeHessian();
                // TODO:: 这个判断条件可以丢掉，条件 b_max <= 1e-12 很难达到，这里的阈值条件不应该用绝对值，而是相对值
//                double b_max = 0.0;
//                for (int i = 0; i < b_.size(); ++i) {
//                    b_max = max(fabs(b_(i)), b_max);
//                }
//                // 优化退出条件2： 如果残差 b_max 已经很小了，那就退出
//                stop = (b_max <= 1e-12);
                false_cnt = 0;
            } else {
                false_cnt++;
                RollbackStates();   // 误差没下降，回滚
            }
        }
        iter++;

        // 优化退出条件3： currentChi_ 跟第一次的chi2相比，下降了 1e6 倍则退出
        if (sqrt(currentChi_) <= stopThresholdLM_)
        {
            std::cout<<GREEN;
            std::cout<<"Residual has been reduced by 1e6 times "<<std::endl;
            std::cout<<RESET;
            stop = true;
        }
    }
    std::cout << "problem solve cost: " << t_solve.toc() << " ms" << std::endl;
    std::cout << "   makeHessian cost: " << t_hessian_cost_ << " ms" << std::endl;
    return true;
}


void Problem::SetOrdering() {

    // 每次重新计数
    ordering_poses_ = 0;
    ordering_generic_ = 0;
    ordering_landmarks_ = 0;

    // Note:: verticies_ 是 map 类型的, 顺序是按照 id 号排序的
    // 统计带估计的所有变量的总维度
    for (auto vertex: verticies_) {
        ordering_generic_ += vertex.second->LocalDimension();  // 所有的优化变量总维数(局部参数化维度之和)
    }
}

void Problem::MakeHessian() {
    TicToc t_h;
    // 直接构造大的 H 矩阵
    ulong size = ordering_generic_;
    MatXX H(MatXX::Zero(size, size));
    VecX b(VecX::Zero(size));

    // TODO:: accelate, accelate, accelate
//#ifdef USE_OPENMP
//#pragma omp parallel for
//#endif

    // 遍历每个残差，并计算他们的雅克比，得到最后的 H = J^T * J
    for (auto &edge: edges_) {

        edge.second->ComputeResidual();    //在edge中的虚函数中实现
        edge.second->ComputeJacobians();

        auto jacobians = edge.second->Jacobians();     //返回值类型vector<MatXX>, vector的size为顶点的数量
        auto verticies = edge.second->Verticies();     //这两个vector的size是等大的．
        assert(jacobians.size() == verticies.size());  //若为假，先打印错误消息，之后abort,终止程序
        for (size_t i = 0; i < verticies.size(); ++i) {
            auto v_i = verticies[i];
            if (v_i->IsFixed()) continue;              // Hessian 里不需要添加它的信息，也就是它的雅克比为 0

            auto jacobian_i = jacobians[i];
            ulong index_i = v_i->OrderingId(); //一个边可有多个顶点,H矩阵是对所有顶点而言的,此变量反映其在H中的位置
            ulong dim_i = v_i->LocalDimension();

            MatXX JtW = jacobian_i.transpose() * edge.second->Information();
            for (size_t j = i; j < verticies.size(); ++j) {
                auto v_j = verticies[j];

                if (v_j->IsFixed()) continue;

                auto jacobian_j = jacobians[j];
                ulong index_j = v_j->OrderingId();
                ulong dim_j = v_j->LocalDimension();

                assert(v_j->OrderingId() != -1);       //从0开始的．
                MatXX hessian = JtW * jacobian_j;      //等同于加权的最小二乘.
                // 所有的信息矩阵叠加起来
                H.block(index_i, index_j, dim_i, dim_j).noalias() += hessian;
                if (j != i) {
                    // 对称的下三角
                    // a=a.transpose()会出现混淆问题https://www.cnblogs.com/defe-learn/p/7456778.html
                    // 确定不会出现混淆，可以使用noalias()来提高效率
                    H.block(index_j, index_i, dim_j, dim_i).noalias() += hessian.transpose();
                }
            }
            b.segment(index_i, dim_i).noalias() -= JtW * edge.second->Residual(); //这里和PPT17页有所出入,这里表示加权的最小二乘，PPT是核函数．
        }

    }
    Hessian_ = H;
    b_ = b;
    t_hessian_cost_ += t_h.toc();

    delta_x_ = VecX::Zero(size);  // initial delta_x = 0_n;

}

/*
* Solve Hx = b, we can use PCG iterative method or use sparse Cholesky
*/
void Problem::SolveLinearSystem() {

        delta_x_ = Hessian_.inverse() * b_;
        //对称正定矩阵的LDLT分解，LM算法由于加上了阻尼系数，可以满足正定的条件．
        //但是牛顿法的H矩阵是半正定的．
        //ldlt()分解的速度维数中小时候很块，精度没有QR分解高
       // delta_x_ = H.ldlt().solve(b_);

}

//更新后的步长deltax加到每一个顶点上
//需要从ordering_generic_的维度中分离出来属于每个顶点的数据
void Problem::UpdateStates() {
    for (auto vertex: verticies_) {                      //vertex是一个pair<id ptr<Vertex>>
        ulong idx = vertex.second->OrderingId();
        ulong dim = vertex.second->LocalDimension();
        VecX delta = delta_x_.segment(idx, dim);

        // 所有的参数 x 叠加一个增量  x_{k+1} = x_{k} + delta_x
        vertex.second->Plus(delta);
    }
}

void Problem::RollbackStates() {
    for (auto vertex: verticies_) {
        ulong idx = vertex.second->OrderingId();
        ulong dim = vertex.second->LocalDimension();
        VecX delta = delta_x_.segment(idx, dim);

        // 之前的增量加了后使得损失函数增加了，我们应该不要这次迭代结果，所以把之前加上的量减去。
        // TODO::频繁的加减损失精度，需要解决,方法同上
        vertex.second->Plus(-delta);
    }
}

/// LM,设定阻尼因子和迭代停止的条件
void Problem::ComputeLambdaInitLM() {
    ni_ = 2.;
    currentLambda_ = -1.;
    currentChi_ = 0.0;
    // TODO:: robust cost chi2
    for (auto edge: edges_) {
        currentChi_ += edge.second->Chi2();         //所有观测误差(二范数)的加权和
    }
    if (err_prior_.rows() > 0)                      //未设置err_prior_，其rows()为0
        currentChi_ += err_prior_.norm();

    stopThresholdLM_ = 1e-6 * currentChi_;          // 迭代条件为 误差下降 1e-6 倍

    double maxDiagonal = 0;
    ulong size = Hessian_.cols();
    assert(Hessian_.rows() == Hessian_.cols() && "Hessian is not square"); //都为false才会出错
    for (ulong i = 0; i < size; ++i) {
        maxDiagonal = std::max(fabs(Hessian_(i, i)), maxDiagonal);
    }
    double tau = 1e-5;
    currentLambda_ = tau * maxDiagonal;
}

//将阻尼因子反映到J'WJ上，对应PPT15页
void Problem::AddLambdatoHessianLM() {
    MatXX I(MatXX::Identity(ordering_generic_,ordering_generic_));
    Hessian_ += currentLambda_*I;

    // 这个操作没有利用已知信息ordering_generic_,循环比较费时.
    // ulong size = Hessian_.cols();
    // assert(Hessian_.rows() == Hessian_.cols() && "Hessian is not square");
    // for (ulong i = 0; i < size; ++i) {
    //     Hessian_(i, i) += currentLambda_;
    // }
}

void Problem::RemoveLambdaHessianLM() {
    //TODO:: 加减损失数值精度.
    MatXX I(MatXX::Identity(ordering_generic_,ordering_generic_));
    Hessian_ -= currentLambda_*I;
    // ulong size = Hessian_.cols();
    // assert(Hessian_.rows() == Hessian_.cols() && "Hessian is not square");
    // // TODO:: 这里不应该减去一个，数值的反复加减容易造成数值精度出问题？而应该保存叠加lambda前的值，在这里直接赋值
    // for (ulong i = 0; i < size; ++i) {
    //     Hessian_(i, i) -= currentLambda_;
    // }
}


/**
 * @brief 阻尼因子的更新策略Nielsen策略PPT20页
 *
 * @return 若跟新变量后，代价函数下降则返回true，否则返回false.
 */
bool Problem::IsGoodStepInLM() {
    double scale = 0;
    // PPT17页公式(10)的分母
    scale = 0.5*delta_x_.transpose() * (currentLambda_ * delta_x_ + b_);
    scale += 1e-6;    // make sure it's non-zero :)

    // recompute residuals after update state
    // 统计所有的残差
    double tempChi = 0.0;
    for (auto edge: edges_) {
        edge.second->ComputeResidual();
        tempChi += edge.second->Chi2();
    }

    double rho = (currentChi_ - tempChi) / scale;   //对应PPT17页公式(10)
    if (rho > 0 && isfinite(tempChi))               // last step was good, 误差在下降,isfinite是有限的
    {
        double alpha = 1. - pow((2 * rho - 1), 3);
        // alpha = std::min(alpha, 2. / 3.);
        // double scaleFactor = (std::max)(1. / 3., alpha);
        double scaleFactor = std::max(1. / 3., alpha);
        currentLambda_ *= scaleFactor;
        ni_ = 2;
        currentChi_ = tempChi;
        return true;
    } else {
        currentLambda_ *= ni_;
        ni_ *= 2;
        return false;
    }
}

/** @brief conjugate gradient with perconditioning
*
*  the jacobi PCG method
*
*/
VecX Problem::PCGSolver(const MatXX &A, const VecX &b, int maxIter = -1) {
    assert(A.rows() == A.cols() && "PCG solver ERROR: A is not a square matrix");
    int rows = b.rows();
    int n = maxIter < 0 ? rows : maxIter;
    VecX x(VecX::Zero(rows));
    MatXX M_inv = A.diagonal().asDiagonal().inverse();
    VecX r0(b);  // initial r = b - A*0 = b
    VecX z0 = M_inv * r0;
    VecX p(z0);
    VecX w = A * p;
    double r0z0 = r0.dot(z0);
    double alpha = r0z0 / p.dot(w);
    VecX r1 = r0 - alpha * w;
    int i = 0;
    double threshold = 1e-6 * r0.norm();
    while (r1.norm() > threshold && i < n) {
        i++;
        VecX z1 = M_inv * r1;
        double r1z1 = r1.dot(z1);
        double belta = r1z1 / r0z0;
        z0 = z1;
        r0z0 = r1z1;
        r0 = r1;
        p = belta * p + z1;
        w = A * p;
        alpha = r1z1 / p.dot(w);
        x += alpha * p;
        r1 -= alpha * w;
    }
    return x;
}

    }
}