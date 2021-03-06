#include "MultiView.hpp"

#include <calib3d.hpp>
#include <cmath>
#include <cstdio>
#include <iostream>

#include "Camera.hpp"
#include "Util.hpp"

namespace MultiView
{
    void fundamental(
        const std::vector<cv::Point2d>& pts1,
        const std::vector<cv::Point2d>& pts2,
        cv::Mat& F,
        std::vector<unsigned char>& inliers)
    {
        using namespace std;
        double maxV1, maxV2;
        cv::minMaxIdx(pts1, 0, &maxV1);
        cv::minMaxIdx(pts2, 0, &maxV2);

        const double maxV = std::max(maxV1, maxV2);
        const double magic      = 0.006;
        const double confidence = 0.99;
        F = cv::findFundamentalMat(
            pts1,
            pts2,
            cv::FM_RANSAC,
            magic * maxV,
            confidence,
            inliers);
    }

    void essential(
        const cv::Mat& F,
        const cv::Mat& K1,
        const cv::Mat& K2,
        cv::Mat& E)
    {
        static cv::Mat W = (cv::Mat_<double>(3, 3) << 0, -1,  0,
                                                      1,  0,  0,
                                                      0,  0,  1);

        static cv::Mat D = (cv::Mat_<double>(3, 3) << 1,  0,  0,
                                                      0,  1,  0,
                                                      0,  0,  0);

        // K2.rows because K2.t()
        assert(K2.rows == F.rows && F.cols == K1.rows);
        E = K2.t() * F * K1;

        cv::Mat w, u, vt;
        cv::SVD::compute(E, w, u, vt);

        assert(u.cols == D.rows && D.cols == vt.rows);
        E = u * D * vt;
    }

    void get_rotation_and_translation(
        const cv::Mat& E,
        std::vector<cv::Mat>& R,
        std::vector<cv::Mat>& T)
    {        
        static cv::Mat W = (cv::Mat_<double>(3, 3) << 0, -1,  0,
                                                      1,  0,  0,
                                                      0,  0,  1);

        cv::Mat w, u, vt;
        cv::SVD::compute(E, w, u, vt);

        assert(u.cols == W.rows && W.rows == vt.cols);
        assert(u.cols == W.cols && W.rows == vt.cols);

        cv::Mat r1, r2, t1, t2;

        r1 = u * W * vt;

        cv::Mat essential = E;
        const int max_iterations = 3;
        int iterations = 0;
        while (Util::eq(determinant(r1), -1.0) && iterations < max_iterations)
        {
            essential = -essential;
            cv::SVD::compute(essential, w, u, vt);
            r1 = u * W * vt;
            ++iterations;
        }

        if (iterations >= max_iterations)
        {
            printf("Exceeded max number of iterations for rotation construction.\n");
            R.clear();
            T.clear();
            return;
        }

        r2 =  u * W.t() * vt;
        t1 =  u.col(2);
        t2 = -u.col(2);

        assert(Util::eq(determinant(r1), 1.0));
        assert(Util::eq(determinant(r2), 1.0));

        assert(r1.size() == cv::Size(3, 3));
        assert(r2.size() == cv::Size(3, 3));
        assert(t1.size() == cv::Size(1, 3));
        assert(t2.size() == cv::Size(1, 3));

        R.push_back(r1);
        T.push_back(t1);

        assert(!Util::eq(determinant(r1), -1.0));

        R.push_back(r1);
        T.push_back(t2);

        R.push_back(r2);
        T.push_back(t1);

        R.push_back(r2);
        T.push_back(t2);
    }

    void get_projection(
        const cv::Mat& R,
        const cv::Mat& T,
        cv::Mat& P)
    {
        P = cv::Mat::zeros(3, 4, CV_64F);
        R.copyTo(P(cv::Rect(0, 0, 3, 3)));
        T.copyTo(P(cv::Rect(3, 0, 1, 3)));
    }

    void transform(
        const cv::Point3d& point,
        const cv::Mat_<double>& rotation,
        const cv::Mat_<double>& translation,
        cv::Point3d& transformed_point)
    {
        cv::Mat_<double> pt = (cv::Mat_<double>(3, 1) << point.x,
                                                         point.y,
                                                         point.z);
        cv::Mat_<double> new_pt = rotation * pt + translation;

        transformed_point = cv::Point3d(new_pt(0), new_pt(1), new_pt(2));
    }

    void triangulate(
        const cv::Point2d& x1,
        const cv::Mat_<double>& P1,
        const cv::Point2d& x2,
        const cv::Mat_<double>& P2,
        cv::Point3d& point)
    {
        cv::Matx43d A;
        for (int i = 0; i <= 2; ++i)
        {
            A(0, i) = x1.x * P1(2, i) - P1(0, i);
            A(1, i) = x1.y * P1(2, i) - P1(1, i);
            A(2, i) = x2.x * P2(2, i) - P2(0, i);
            A(3, i) = x2.y * P2(2, i) - P2(1, i);
        }

        cv::Matx41d B(-(x1.x * P1(2, 3) - P1(0, 3)),
                      -(x1.y * P1(2, 3) - P1(1, 3)),
                      -(x2.x * P2(2, 3) - P2(0, 3)),
                      -(x2.y * P2(2, 3) - P2(1, 3)));

        cv::Mat_<double> _X;
        cv::solve(A, B, _X, cv::DECOMP_SVD);
        point = cv::Point3d(_X(0), _X(1), _X(2));
    }

    void iterative_triangulate(
        const cv::Point2d& x1,
        const cv::Matx34d& P1,
        const cv::Point2d& x2,
        const cv::Matx34d& P2,
        cv::Point3d& point)
    {
        static const int max_iterations = 10;

        double wi1 = 1.0, wi2 = 1.0;
        cv::Mat_<double> X_(4, 1);

        triangulate(x1, cv::Mat(P1), x2, cv::Mat(P2), point);
        cv::Mat_<double> X = (cv::Mat_<double>(4, 1) << point.x,
                                                        point.y,
                                                        point.z,
                                                        1.0);

        for (int i = 0; i < max_iterations; ++i)
        {
            double p2x1 = cv::Mat_<double>(cv::Mat_<double>(P1).row(2) * X)(0);
            double p2x2 = cv::Mat_<double>(cv::Mat_<double>(P2).row(2) * X)(0);
            
            if (Util::eq(wi1, p2x1) && Util::eq(wi2, p2x2)) break;
            
            wi1 = p2x1;
            wi2 = p2x2;
            assert(wi1 != 0.0 and wi2 != 0.0);

            cv::Matx43d A;
            for (int j = 0; j <= 2; ++j)
            {
                A(0, j) = (x1.x * P1(2, j) - P1(0, j)) / wi1;
                A(1, j) = (x1.y * P1(2, j) - P1(1, j)) / wi1;
                A(2, j) = (x2.x * P2(2, j) - P2(0, j)) / wi2;
                A(3, j) = (x2.y * P2(2, j) - P2(1, j)) / wi2;
            }

            cv::Mat_<double> B = (cv::Mat_<double>(4,1) << -(x1.x * P1(2,3) - P1(0,3)) / wi1,
                                                           -(x1.y * P1(2,3) - P1(1,3)) / wi1,
                                                           -(x2.x * P2(2,3) - P2(0,3)) / wi2,
                                                           -(x2.y * P2(2,3) - P2(1,3)) / wi2);
            
            solve(A, B, X_, cv::DECOMP_SVD);
            X(0) = X_(0);
            X(1) = X_(1);
            X(2) = X_(2);
            X(3) = 1.0;
        }

        point = cv::Point3d(X(0), X(1), X(2));
    }

    void triangulate(
        const std::vector<cv::Point2d>& pts1,
        const cv::Mat& K1,
        const std::vector<cv::Point2d>& pts2,
        const cv::Mat& K2,
        const cv::Mat& rotation,
        const cv::Mat& translation,
        std::vector<cv::Point3d>& points)
    {
        assert(rotation.size() == cv::Size(3, 3));
        assert(translation.size() == cv::Size(1, 3));

        cv::Mat no_rotation = cv::Mat::eye(3, 3, CV_64F);
        cv::Mat no_translation = cv::Mat::zeros(3, 1, CV_64F);

        cv::Mat P1, P2;
        get_projection(no_rotation, no_translation, P1);
        get_projection(rotation, translation, P2);

        assert(K1.cols == P1.rows && K2.cols == P2.rows);
        triangulate(pts1, P1, K1, pts2, P2, K2, points);
    }

    void triangulate(
        const std::vector<cv::Point2d>& pts1,
        const cv::Mat& P1,
        const cv::Mat& K1,
        const std::vector<cv::Point2d>& pts2,
        const cv::Mat& P2,
        const cv::Mat& K2,
        std::vector<cv::Point3d>& points)
    {
        using namespace std;
        assert(P1.size() == cv::Size(4, 3));
        assert(P2.size() == cv::Size(4, 3));
        assert(K1.size() == cv::Size(3, 3));
        assert(K2.size() == cv::Size(3, 3));
        assert(P1.type() == CV_64F);
        assert(P2.type() == CV_64F);
        assert(K1.type() == CV_64F);
        assert(K2.type() == CV_64F);
        assert(pts1.size() == pts2.size());

        const cv::Mat K1_inv = K1.inv();
        const cv::Mat K2_inv = K2.inv();
        const cv::Mat KP1 = K1 * P1;
        const cv::Mat KP2 = K2 * P2;

        const int n = pts1.size();
        points = std::vector<cv::Point3d>(n);
        for (int i = 0; i < n; ++i)
        {
            cv::Mat_<double> p1 = (cv::Mat_<double>(3, 1) << pts1[i].x,
                                                             pts1[i].y,
                                                             1.0);
            cv::Mat_<double> p2 = (cv::Mat_<double>(3, 1) << pts2[i].x,
                                                             pts2[i].y,
                                                             1.0);
            cv::Mat_<double> np1 = K1_inv * p1;
            cv::Mat_<double> np2 = K2_inv * p2;

            // Ignore the last coordinate.
            assert(Util::eq(np1(2), 1.0) and Util::eq(np2(2), 1.0));

            cv::Point2d x1(np1(0), np1(1));
            cv::Point2d x2(np2(0), np2(1));

            iterative_triangulate(x1, P1, x2, P2, points[i]);
        }
    }

    void triangulate(
        const std::vector<cv::Point2d>& pts1,
        const cv::Mat& K1,
        const std::vector<cv::Point2d>& pts2,
        const cv::Mat& K2,
        std::vector<unsigned char>& inliers,
        std::vector<cv::Point3d>& points)
    {
        static const double min_percent_in_front = 0.75;

        assert(pts1.size() == pts2.size());

        cv::Mat F, E;
        fundamental(pts1, pts2, F, inliers);
        essential(F, K1, K2, E);

        assert(inliers.size() == pts1.size());
        assert(inliers.size() == pts2.size());

        std::vector<cv::Point2d> best_pts1, best_pts2;
        Util::mask(pts1, inliers, best_pts1);
        Util::mask(pts2, inliers, best_pts2);

        std::vector<int> indices;
        for (int i = 0; i < inliers.size(); ++i)
        {
            indices.push_back(i);
        }

        std::vector<int> best_indices;
        Util::mask(indices, inliers, best_indices);

        std::vector<cv::Mat> rotations, translations;
        get_rotation_and_translation(E, rotations, translations);

        int n = rotations.size();
        std::vector<std::vector<cv::Point3d> > clouds(n);
        std::vector<std::vector<uchar> > in_front_inliers(n);
        std::vector<double> in_front_percent1(n), in_front_percent2(n);
        std::vector<double> projection_error1(n), projection_error2(n);

        for (int i = 0; i < n; ++i)
        {
            cv::Mat rotation = rotations[i];
            cv::Mat translation = translations[i];

            cv::Mat no_rotation = cv::Mat::eye(3, 3, CV_64F);
            cv::Mat no_translation = cv::Mat::zeros(3, 1, CV_64F);
            
            triangulate(
                best_pts1,
                K1,
                best_pts2,
                K2,
                rotation,
                translation,
                clouds[i]);

            std::vector<cv::Point2d> projected_pts1, projected_pts2;
            project(clouds[i], no_rotation, no_translation, K1, projected_pts1);
            project(clouds[i], rotation, translation, K2, projected_pts2);

            double in_front1 = 0.0, in_front2 = 0.0;
            double proj_err1 = 0.0, proj_err2 = 0.0;

            int num_points = best_pts1.size();
            for (int j = 0; j < num_points; ++j)
            {
                cv::Point3d pt3d1, pt3d2;
                pt3d1 = clouds[i][j];
                transform(pt3d1, rotation, translation, pt3d2);

                in_front1 += pt3d1.z > 0.0;
                in_front2 += pt3d2.z > 0.0;
                bool in_front = pt3d1.z > 0.0 and pt3d2.z > 0.0;
                in_front_inliers[i].push_back(in_front);

                proj_err1 += cv::norm(cv::Mat(best_pts1[j]), cv::Mat(projected_pts1[j]));
                proj_err2 += cv::norm(cv::Mat(best_pts2[j]), cv::Mat(projected_pts2[j]));
            }

            assert(num_points != 0);
            in_front1 /= num_points;
            in_front2 /= num_points;
            proj_err1 /= num_points;
            proj_err2 /= num_points;

            in_front_percent1[i] = in_front1;
            in_front_percent2[i] = in_front2;
            projection_error1[i] = proj_err1;
            projection_error2[i] = proj_err2;
        }


        int best_index = -1;
        for (int i = 0; i < 4; ++i)
        {
            if (in_front_percent1[i] > min_percent_in_front &&
                in_front_percent2[i] > min_percent_in_front)
            {
                best_index = i;
                break;
            }
        }

        std::cout << best_index << std::endl;
        for (int i = 0; i < 4; ++i)
        {
            printf("%ld: (%f, %f) and (%f, %f)\n",
                clouds[i].size(),
                in_front_percent1[i],
                in_front_percent2[i],
                projection_error1[i],
                projection_error2[i]);
        }


        if (best_index == -1)
        {
            printf("Couldn't find a proper cloud.\n");
            return;
        }

        Util::mask(clouds[best_index], in_front_inliers[best_index], points);

        for (int i = 0; i < in_front_inliers[best_index].size(); ++i)
        {
            inliers[best_indices[i]] &= in_front_inliers[best_index][i];
        }

        assert(cv::countNonZero(inliers) == points.size());
    }

    void project(
        const cv::Point3d& point,
        const cv::Mat& rotation,
        const cv::Mat& translation,
        const cv::Mat& camera,
        cv::Point2d& coordinate)
    {
        cv::Mat projection;
        get_projection(rotation, translation, projection);

        assert(camera.cols == projection.rows);
        project(point, camera * projection, coordinate);
        // project(point, projection, coordinate);
    }

    void project(
        const cv::Point3d& point,
        const cv::Mat& projection,
        cv::Point2d& coordinate)
    {
        assert(projection.size() == cv::Size(4, 3));

        cv::Mat pt = (cv::Mat_<double>(4, 1) << point.x,
                                                point.y,
                                                point.z,
                                                1.0);

        cv::Mat proj_pt = projection * pt;
        double wx = proj_pt.at<double>(0);
        double wy = proj_pt.at<double>(1);
        double w = proj_pt.at<double>(2);

        assert(w != 0.0);
        double x = wx / w;
        double y = wy / w;

        coordinate = cv::Point2d(x, y);
    }

    void project(
        const std::vector<cv::Point3d>& points,
        const cv::Mat& rotation,
        const cv::Mat& translation,
        const cv::Mat& camera,
        std::vector<cv::Point2d>& coordinates)
    {
        cv::Mat projection;
        get_projection(rotation, translation, projection);

        assert(camera.cols == projection.rows);
        project(points, camera * projection, coordinates);
        // project(points, projection, coordinates);
    }

    void project(
        const std::vector<cv::Point3d>& points,
        const cv::Mat& projection,
        std::vector<cv::Point2d>& coordinates)
    {
        int n = points.size();
        coordinates = std::vector<cv::Point2d>(n);
        for (int i = 0; i < n; ++i)
        {
            project(points[i], projection, coordinates[i]);
        }
    }
}
