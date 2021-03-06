#include <opencv.hpp>

#include <string>

using namespace std;
using namespace cv;

int main(int argc, char** argv)
{
    if (argc < 3)
        return 0;

    string directory = argv[1];
    int index = atoi(argv[2]);

    if (directory[directory.size() - 1] != '/')
        directory += '/';

    Mat image;
    VideoCapture vc(index);

    if (!vc.isOpened()) return 0;

    int i = 0;
    while (vc.isOpened())
    {
        vc >> image;
        imshow("image", image);
        char c = waitKey(20);
        if (c == ' ')
            imwrite(directory + to_string(i++) + ".jpg", image);
        else if (c == 27)
            break;
    }
}
