/*
Copyright (c) 2022, Fei Hou and Chiyu Wang, Institute of Software, Chinese Academy of Sciences.
All rights reserved.

The code can only be used for academic purpose, and cannot be used for commercial
purpose without written permission.

Redistribution and use in source for academic purpose, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of
conditions and the following disclaimer.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.
*/

#include <vector>
#include <string>
#include <queue>
#include <algorithm>
#include "kdtree.h"
#include "utility.h"
#include "PoissonRecon.h"

using namespace std;

void ipsr(const string &input_name, const string &output_name, int iters, double pointweight, int depth, int k_neighbors)
{
	typedef double REAL;
	const unsigned int DIM = 3U;

	vector<pair<Point<REAL, DIM>, Normal<REAL, DIM>>> points_normals;
	ply_reader<REAL, DIM>(input_name, points_normals);

	string command = "PoissonRecon --in i.ply --out o.ply --bType 2 --depth " + to_string(depth) + " --pointWeight " + to_string(pointweight);
	vector<string> cmd = split(command);
	vector<char *> argv_str(cmd.size());
	for (size_t i = 0; i < cmd.size(); ++i)
		argv_str[i] = &cmd[i][0];

	XForm<REAL, DIM + 1> iXForm;
	vector<double> weight_samples;
	// sample points by the octree
	points_normals = sample_points<REAL, DIM>((int)argv_str.size(), argv_str.data(), points_normals, iXForm, &weight_samples); // 通过octree，得到 n 个点。

	// initialize normals randomly
	printf("random initialization...\n");
	Normal<REAL, DIM> zero_normal(Point<REAL, DIM>(0, 0, 0));
	srand(0);
	for (size_t i = 0; i < points_normals.size(); ++i)
	{
		do
		{
			points_normals[i].second = Point<REAL, DIM>(rand() % 1001 - 500.0, rand() % 1001 - 500.0, rand() % 1001 - 500.0); // 随机生成一个法向量，大概是[234,178,762]形式
		} while (points_normals[i].second == zero_normal);
		normalize<REAL, DIM>(points_normals[i].second); // 然后将其模正则到 1。
	}

	// construct the Kd-Tree
	kdt::KDTree<kdt::KDTreePoint> tree;
	{
		vector<kdt::KDTreePoint> vertices;
		vertices.reserve(points_normals.size());
		for (size_t i = 0; i < points_normals.size(); ++i)
		{
			array<double, 3> p{points_normals[i].first[0], points_normals[i].first[1], points_normals[i].first[2]};
			vertices.push_back(kdt::KDTreePoint(p)); // 将octree 采样得到的点放置到 vertices 中。
		}
		tree.build(vertices); // 利用 vertices 来构建 octree。
	}

	pair<vector<Point<REAL, DIM>>, vector<vector<int>>> mesh;

	// iterations
	int epoch = 0;
	while (epoch < iters)
	{
		++epoch;
		printf("Iter: %d\n", epoch);

		vector<Point<REAL, DIM>>().swap(mesh.first);
		vector<vector<int>>().swap(mesh.second);

		// Poisson reconstruction
		mesh = poisson_reconstruction<REAL, DIM>((int)argv_str.size(), argv_str.data(), points_normals, &weight_samples);

		vector<vector<int>> nearestSamples(mesh.second.size()); // 用于存放每个面的 K 个近邻 sample 点。
		vector<Point<REAL, DIM>> normals(mesh.second.size()); // 用于存放每个面的法向量。

		// compute face normals and map them to sample points
#pragma omp parallel for
		for (int i = 0; i < (int)nearestSamples.size(); i++)
		{
			if (mesh.second[i].size() == 3)
			{
				Point<REAL, DIM> c = mesh.first[mesh.second[i][0]] + mesh.first[mesh.second[i][1]] + mesh.first[mesh.second[i][2]]; // 得到面片三个顶点的和
				c /= 3; // 然后除以 3，得到面片的中心点坐标
				array<REAL, DIM> a{c[0], c[1], c[2]};
				nearestSamples[i] = tree.knnSearch(kdt::KDTreePoint(a), k_neighbors);
				normals[i] = Point<REAL, DIM>::CrossProduct(mesh.first[mesh.second[i][1]] - mesh.first[mesh.second[i][0]], mesh.first[mesh.second[i][2]] - mesh.first[mesh.second[i][0]]); // 计算面的法向量（这里的法向量因为没有归一化，因此其实相当云面积加权之后的法向量）
			}
		}

		// update sample point normals
		vector<Normal<REAL, DIM>> projective_normals(points_normals.size(), zero_normal); // 用[0,0,0]来初始化每一个 sample 点的映射向量。
		for (size_t i = 0; i < nearestSamples.size(); i++)
		{
			for (size_t j = 0; j < nearestSamples[i].size(); ++j)
			{
				projective_normals[nearestSamples[i][j]].normal[0] += normals[i][0]; // 将每一个面的法向量贡献给与它产生联系的 sample 点。下同。
				projective_normals[nearestSamples[i][j]].normal[1] += normals[i][1];
				projective_normals[nearestSamples[i][j]].normal[2] += normals[i][2];
			}
		}

#pragma omp parallel for
		for (int i = 0; i < (int)projective_normals.size(); ++i)
			normalize<REAL, DIM>(projective_normals[i]); // 将这些 sample 点的映射向量的模正则为 1。

		// compute the average normal variation of the top 1/1000 points
		size_t heap_size = static_cast<size_t>(ceil(points_normals.size() / 1000.0));
		priority_queue<double, vector<double>, greater<double>> min_heap;
		for (size_t i = 0; i < points_normals.size(); ++i)
		{
			if (!(projective_normals[i] == zero_normal)) // 如果 sample 的 normal 不为0，那我们计算它的 normal 与之前状态的变化量。
			{
				double diff = Point<REAL, DIM>::SquareNorm((projective_normals[i] - points_normals[i].second).normal);
				if (min_heap.size() < heap_size)
					min_heap.push(diff);
				else if (diff > min_heap.top()) // 找到 1/1000 * sample.size 个法向量变化最大的值。
				{
					min_heap.pop();
					min_heap.push(diff);
				}

				points_normals[i].second = projective_normals[i]; // 用 sample 的映射 normal 向量来更新自身的 normal。
			}
		}

		heap_size = min_heap.size();
		double ave_max_diff = 0;
		while (!min_heap.empty())
		{
			ave_max_diff += sqrt(min_heap.top());
			min_heap.pop();
		}
		ave_max_diff /= heap_size;
		printf("normals variation %f\n", ave_max_diff);
		if (ave_max_diff < 0.175)
			break;
	}

	mesh = poisson_reconstruction<REAL, DIM>((int)argv_str.size(), argv_str.data(), points_normals, &weight_samples);

	output_ply(output_name, mesh, iXForm);
	// output_sample_points_and_normals<REAL, DIM>("points_normals_samples.ply", points_normals, iXForm);
	// output_all_points_and_normals<REAL, DIM>("points_normals_all.ply", input_name, points_normals, tree, iXForm);
}

int main(int argc, char *argv[])
{
	string input_name, output_name;
	int iters = 30;
	double pointweight = 10;
	int depth = 10;
	int k_neighbors = 10;
	for (int i = 1; i < argc; i += 2)
	{
		if (strcmp(argv[i], "--in") == 0)
		{
			input_name = argv[i + 1];
			string extension = input_name.substr(min<size_t>(input_name.find_last_of('.'), input_name.length()));
			for (size_t i = 0; i < extension.size(); ++i)
				extension[i] = tolower(extension[i]);
			if (extension != ".ply")
			{
				printf("The input shoud be a .ply file\n");
				return 0;
			}
		}
		else if (strcmp(argv[i], "--out") == 0)
		{
			output_name = argv[i + 1];
			string extension = output_name.substr(min<size_t>(output_name.find_last_of('.'), output_name.length()));
			for (size_t i = 0; i < extension.size(); ++i)
				extension[i] = tolower(extension[i]);
			if (extension != ".ply")
			{
				printf("The output shoud be a .ply file\n");
				return 0;
			}
		}
		else if (strcmp(argv[i], "--iters") == 0)
		{
			long v = strtol(argv[i + 1], nullptr, 10);
			if (!valid_parameter(v))
			{
				printf("invalid value of --iters");
				return 0;
			}
			iters = static_cast<int>(v);
		}
		else if (strcmp(argv[i], "--pointWeight") == 0)
		{
			pointweight = strtod(argv[i + 1], nullptr);
			if (pointweight < 0.0 || pointweight == HUGE_VAL || pointweight == -HUGE_VAL)
			{
				printf("invalid value of --pointWeight");
				return 0;
			}
		}
		else if (strcmp(argv[i], "--depth") == 0)
		{
			long v = strtol(argv[i + 1], nullptr, 10);
			if (!valid_parameter(v))
			{
				printf("invalid value of --depth");
				return 0;
			}
			depth = static_cast<int>(v);
		}
		else if (strcmp(argv[i], "--neighbors") == 0)
		{
			long v = strtol(argv[i + 1], nullptr, 10);
			if (!valid_parameter(v))
			{
				printf("invalid value of --neighbors");
				return 0;
			}
			k_neighbors = static_cast<int>(v);
		}
		else
		{
			printf("unknown parameter of %s\n", argv[i]);
			return 0;
		}
	}

	if (argc <= 1 || input_name.empty() || output_name.empty())
	{
		printf("Parameters:\n");
		printf("--in                      input .ply model\n");
		printf("--out                     output .ply model\n");
		printf("--iters (optional)        maximum number of iterations, default 30\n");
		printf("--pointWeight (optional)  screened weight of SPSR, default 10\n");
		printf("--depth (optional)        maximum depth of the octree, default 10\n");
		printf("--neighbors (optional)    number of the nearest neighbors to search, default 10\n");
		return 0;
	}

	printf("Iterative Poisson Surface Reconstruction (iPSR)\n");
	printf("Parameters:\n");
	printf("--in          %s\n", input_name.c_str());
	printf("--out         %s\n", output_name.c_str());
	printf("--iters       %d\n", iters);
	printf("--pointWeight %f\n", pointweight);
	printf("--depth       %d\n", depth);
	printf("--neighbors   %d\n\n", k_neighbors);

	ipsr(input_name, output_name, iters, pointweight, depth, k_neighbors);

	return 0;
}
