# TSPTW

本リポジトリは [Domain-Independent Dynamic Programming](https://arxiv.org/abs/2401.13883) (Ryo Kuroiwa and J. Christopher Beck) における TSPTW の実験に基づく．ソルバーの実装は [didp-models](https://github.com/Kurorororo/didp-models/tree/main) に由来する．

Traveling Salesman Problem with Time Windows (TSPTW) は，デポ 0 と顧客集合 N = {1, ..., n-1} が与えられ，各顧客 i に時間枠 [a_i, b_i]，各辺 (i, j) に移動時間 c_ij が定義される．デポを出発し，すべての顧客を時間枠内にちょうど1回ずつ訪問し，デポに戻る最小コストの巡回路を求める．

- 到着時刻が a_i より早い場合は a_i まで待機する
- 到着時刻が b_i を超える訪問は許されない

### 目的関数

2つの評価指標がある（[参照](https://lopez-ibanez.eu/tsptw-instances)）:

- **Travel time**: 移動時間の総和 $\sum c_{ij}$（待ち時間を含まない）
- **Makespan**: デポへの帰還時刻（待ち時間を含む総経過時間）

本リポジトリでは travel time を対象とする．

## 入力フォーマット

```
n
c_00 c_01 ... c_0(n-1)
c_10 c_11 ... c_1(n-1)
...
c_(n-1)0 c_(n-1)1 ... c_(n-1)(n-1)
a_0 b_0
a_1 b_1
...
a_(n-1) b_(n-1)
```

- 1行目: ノード数 n（デポ + 顧客数）
- 続く n 行: n x n の移動時間行列（対角要素は無視）
- 続く n 行: 各ノードの時間枠 [a_i, b_i]

## 例

4 ノード（デポ 0，顧客 1, 2, 3）の非対称インスタンス（c_03 = 4, c_30 = 7）:

```
4
0 3 5 4
3 0 2 6
5 2 0 3
7 6 3 0
0 100
5 20
8 12
10 30
```

![TSPTW example](example.png)

時間枠を無視した TSP の最適解は 0 -> 3 -> 2 -> 1 -> 0（コスト = 4 + 3 + 2 + 3 = 12）だが，顧客 2 に時刻 13 に到着し時間枠 [8, 12] に違反するため実行不可能．

TSPTW 最適解: 0 -> 1 -> 2 -> 3 -> 0

| 訪問先 | 移動時間 | 到着時刻 | 時間枠 | 出発時刻 |
|--------|----------|----------|--------|----------|
| 0 (デポ) | - | 0 | [0, 100] | 0 |
| 1 | 3 | 5 (待機) | [5, 20] | 5 |
| 2 | 2 | 8 (待機) | [8, 12] | 8 |
| 3 | 3 | 11 | [10, 30] | 11 |
| 0 (帰還) | 7 | 18 | [0, 100] | - |

- Travel time = 3 + 2 + 3 + 7 = **15**（移動時間の総和）
- Makespan = **18**（デポ帰還時刻、待ち時間を含む）


## ソルバー実行コマンド

各ソルバーは `solvers/<solver>/` ディレクトリから実行する．

### MIP (Gurobi)

```bash
cd solvers/mip
python3 tsptw_mip.py ../../benchmark/AFG/rbg010a.tw --time-out 1800 --history history.csv
```

### CP (CP Optimizer)

```bash
cd solvers/cp
python3 tsptw_cp.py ../../benchmark/AFG/rbg010a.tw --cpoptimizer /path/to/cpoptimizer --time-out 1800 --history history.csv
```

### DyPDL (CABS)

```bash
cd solvers/didp
python3 tsptw_didp.py ../../benchmark/AFG/rbg010a.tw --config CABS --time-out 1800 --history history.csv
```

### OR-Tools (CP-SAT)

```bash
cd solvers/ortools
python3 tsptw_ortools.py ../../benchmark/AFG/rbg010a.tw --time-out 1800
```

### Picat (Branch & Bound)

[Picat](http://picat-lang.org/) のインストールが必要．

```bash
cd solvers/picat
picat tsptw_bb.pi ../../benchmark/AFG/rbg010a.tw
```

### テスト実行結果

`benchmark/AFG/rbg010a.tw`（11ノード）での実行結果:

| ソルバー | 最適コスト | 実行時間 |
|---|---:|---:|
| MIP (Gurobi) | 671 | 0.02s |
| CP (CP Optimizer) | 671 | 33.65s |
| DyPDL (CABS) | 671 | 0.001s |
| OR-Tools (CP-SAT) | 671 | 0.20s |

## 制約モデルの例

### MIP モデル

Hungerländer and Truden [10] の Formulation (1) に基づく．ゼロコスト辺が存在する場合，フローベースの部分巡回除去制約 [132] を追加する．

#### 決定変数

- $x_{ij} \in \{0, 1\}$: 辺 $(i, j)$ を使用するか ($\forall (i, j) \in E$)
- $t_i \in [a_i, b_i]$: 顧客 $i$ への到着時刻 ($\forall i \in N$)
- $t_n$: デポへの帰還時刻

#### 目的関数

$$\min \sum_{(i,j) \in E} c_{ij} x_{ij}$$

#### 制約

$$\sum_{i: (i,j) \in E, i \neq j} x_{ij} = 1 \quad \forall j \in \{0\} \cup N$$

$$\sum_{j: (i,j) \in E, j \neq i} x_{ij} = 1 \quad \forall i \in \{0\} \cup N$$

$$t_i - c_{0i} \cdot x_{0i} \geq 0 \quad \forall i \in N$$

$$t_i - t_j + (b_i - a_j + c_{ij}) x_{ij} \leq b_i - a_j \quad \forall (i, j) \in E,\ i, j \in N$$

$$t_i + c_{i0} \leq t_n \quad \forall i \in N$$

#### 部分巡回除去制約（フローベース [132]）

ゼロコスト辺が存在する場合に追加:

- $y_{ij} \geq 0$: フロー変数

$$y_{ij} \leq (n-1) x_{ij} \quad \forall (i, j) \in E$$

$$\sum_{j \in N} y_{0j} = n - 1$$

$$\sum_{i: (i,j) \in E, i \neq j} y_{ij} - \sum_{k: (j,k) \in E, k \neq j} y_{jk} = 1 \quad \forall j \in N$$

#### 部分巡回除去制約（MTZ，代替）

- $w_i \geq 0$: 順序変数

$$w_i - w_j + (n-1) x_{ij} \leq n - 2 \quad \forall i, j \in N,\ i \neq j$$

### CP モデル

Booth et al. [11] の単一機械スケジューリング問題（時間枠・順序依存段取り時間付き）のCPモデルを TSPTW に適応．CP Optimizer (docplex.cp) を使用．

#### 変数

- $x_i$: 顧客 $i$ の訪問を表すインターバル変数 ($\forall i \in \{0\} \cup N$)
  - デポ ($i = 0$): $\text{start} = 0$, $\text{length} = 0$
  - 顧客 ($i \in N$): $\text{start} \in [a_i, +\infty)$, $\text{end} \in [a_i, b_i]$, $\text{length} = 0$
- $\pi$: $x_i$ の順序を表すシーケンス変数（型 = ノード番号）

#### 遷移距離行列

$$D[i][j] = c_{ij}$$

#### 制約

$$\texttt{no\_overlap}(\pi, D, \text{is\_direct} = \text{true})$$

$$\texttt{first}(\pi, x_0)$$

- `no_overlap`: シーケンス中のインターバルが重ならず，連続する要素間に遷移距離行列で定義された最小距離を確保する（段取り時間に対応）
- `first`: デポ $x_0$ をシーケンスの先頭に固定（[11] にはない TSPTW 用の追加制約）

#### 目的関数

$$\min \sum_{i \in \{0\} \cup N} D[i][\texttt{type\_of\_next}(\pi, x_i, 0)]$$

`type_of_next`$(\pi, x_i, 0)$ はシーケンス $\pi$ 中で $x_i$ の次の要素の型（ノード番号）を返す．最後の要素の場合はデフォルト値 $0$（デポ）を返す．

#### [11] からの変更点

1. **目的関数の変更**: [11] では段取り時間の和をメイクスパンの一部として最小化するが，TSPTW では移動コストの総和を直接最小化する
2. **First 制約の追加**: デポを最初に訪問する制約を追加（[11] にはない TSPTW 固有の制約）

### OR-Tools CP-SAT モデル

Google OR-Tools の CP-SAT ソルバーを使用．位置変数による定式化．

#### 決定変数

- $p_i \in \{0, \ldots, n-1\}$: ノード $i$ の巡回順序（$\forall i \in \{0\} \cup N$）
- $t_i \in [0, T_{\max}]$: ノード $i$ への到着時刻（$\forall i \in \{0\} \cup N$）

ここで $T_{\max} = \max_i b_i + \max_{(i,j) \in E} c_{ij}$．

#### 制約

$$p_0 = 0$$

$$t_0 = 0$$

$$\texttt{AllDifferent}(p_0, p_1, \ldots, p_{n-1})$$

$$a_i \leq t_i \leq b_i \quad \forall i \in \{0\} \cup N$$

$$p_j = p_i + 1 \Rightarrow t_j \geq t_i + c_{ij} \quad \forall i, j \in \{0\} \cup N,\ i \neq j$$

#### 目的関数

$$\min \sum_{k=0}^{n-2} \sum_{\substack{i,j: \\ p_i = k,\ p_j = k+1}} c_{ij} + \sum_{\substack{i: \\ p_i = n-1}} c_{i0}$$

実装上は，各位置 $k$ と各辺 $(i,j)$ に対して「$p_i = k$ かつ $p_j = k+1$」を表すブール変数を導入し，条件付き制約（`OnlyEnforceIf`）で辺コストを合計する．最後の位置からデポへの帰還コストも同様に処理する．

### Picat モデル

Picat の `planner` モジュールによる分枝限定探索（`best_plan_bb`）．状態 $(U, l, \tau)$ を用いる．

#### 状態変数

- $U \subseteq N$: 未訪問の顧客集合（初期値: $N$）
- $l \in \{0\} \cup N$: 現在地（初期値: $0$）
- $\tau \in \mathbb{Z}_{\geq 0}$: 現在時刻（初期値: $0$）

#### 終了条件

$$U = \emptyset$$

帰還コスト $c_{l0}$ を加算してデポに戻る．

#### アクション: visit $j$ ($\forall j \in N$)

- 事前条件: $j \in U \land \tau + c_{lj} \leq b_j \land \text{constraint}(U \setminus \{j\}, j, \max(\tau + c_{lj},\ a_j))$
- 効果:
  - $U \leftarrow U \setminus \{j\}$
  - $l \leftarrow j$
  - $\tau \leftarrow \max(\tau + c_{lj},\ a_j)$
- コスト: $c_{lj}$

#### 状態制約

$$\forall j \in U: \tau + d^*_{lj} \leq b_j$$

$d^*_{ij}$ はデポを経由しない最短距離（Floyd–Warshall で中間頂点を顧客のみに制限して計算）．到達不可能な顧客が残る状態を枝刈りする．

#### ヒューリスティック（双対限界）

$$H_1 = \sum_{j \in U} \min_{i \neq j} c_{ij} + \mathbb{1}[l \neq 0] \cdot \min_{i \neq 0} c_{i0}$$

$$H_2 = \sum_{j \in U} \min_{k \neq j} c_{jk} + \mathbb{1}[l \neq 0] \cdot \min_{k \neq l} c_{lk}$$

$$H = \max(H_1, H_2)$$

$H_1$ は各未訪問頂点への最小入辺コストの和（+ デポへの最小入辺），$H_2$ は各未訪問頂点からの最小出辺コストの和（+ 現在地からの最小出辺）．

### モデルの参考文献

- [10] P. Hungerländer, C. Truden, Efficient and easy-to-implement mixed-integer linear programs for the traveling salesman problem with time windows, Transp. Res. Procedia 30 (2018) 157–166.
- [11] K. E. Booth, T. T. Tran, G. Nejat, J. C. Beck, Mixed-integer and constraint programming techniques for mobile robot task planning, IEEE Robot. Autom. Lett. 1 (2016) 500–507.
- [23] Y. Dumas, J. Desrosiers, E. Gelinas, M. M. Solomon, An optimal algorithm for the traveling salesman problem with time windows, Oper. Res. 43 (1995) 367–371.
- [129] M. Gendreau, A. Hertz, G. Laporte, M. Stan, A generalized insertion heuristic for the traveling salesman problem with time windows, Oper. Res. 46 (1998) 330–346.
- [130] J. W. Ohlmann, B. W. Thomas, A compressed-annealing heuristic for the traveling salesman problem with time windows, INFORMS J. Comput. 19 (2007) 80–90.
- [131] N. Ascheuer, Hamiltonian Path Problems in the On-Line Optimization of Flexible Manufacturing Systems, Ph.D. thesis, Technische Universität Berlin, 1995.
- [132] (フローベース部分巡回除去制約)

## ベンチマークインスタンス

[Benchmark instances](https://lopez-ibanez.eu/tsptw-instances) から取得した **340 インスタンス**（4つのベンチマークセット）．

### AFG — 50 インスタンス

非対称TSPTW[^asym]．ファイル名 `rbgXXX[a-d][.N].tw`．サイズが多様．

[^asym]: 非対称 = 距離行列が対称でない（d(A→B) ≠ d(B→A)）．他の3セット (Dumas, Gendreau-Dumas Ext., Ohlmann-Thomas) は対称（d(A→B) = d(B→A)）．

| ノード数 N | 都市数 | インスタンス数 | ファイル例 |
|---:|---:|---:|:---|
| 11 | 10 | 1 | rbg010a |
| 16 | 15 | 2 | rbg017, rbg017.2 |
| 17 | 16 | 2 | rbg016a, rbg016b |
| 18 | 17 | 1 | rbg017a |
| 20 | 19 | 13 | rbg019a–d, rbg021–.9 |
| 21 | 20 | 1 | rbg020a |
| 28–56 | 27–55 | 14 | rbg027a–rbg055a |
| 68–93 | 67–92 | 3 | rbg067a, rbg086a, rbg092a |
| 126–232 | 125–231 | 13 | rbg125a–rbg233.2 |

### Dumas — 135 インスタンス

各 (n, w) 組に 5 インスタンス．n=都市数, w=時間枠幅．狭い時間枠 (w=20–100)．

| n \ w | 20 | 40 | 60 | 80 | 100 |
|---:|:---:|:---:|:---:|:---:|:---:|
| **20** | 5 | 5 | 5 | 5 | 5 |
| **40** | 5 | 5 | 5 | 5 | 5 |
| **60** | 5 | 5 | 5 | 5 | 5 |
| **80** | 5 | 5 | 5 | 5 | - |
| **100** | 5 | 5 | 5 | - | - |
| **150** | 5 | 5 | 5 | - | - |
| **200** | 5 | 5 | - | - | - |

### Gendreau-Dumas Extended — 130 インスタンス

各 (n, w) 組に 5 インスタンス．Dumas セットの補完（広い時間枠 w=80–200）．

| n \ w | 80 | 100 | 120 | 140 | 160 | 180 | 200 |
|---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **20** | - | - | 5 | 5 | 5 | 5 | 5 |
| **40** | - | - | 5 | 5 | 5 | 5 | 5 |
| **60** | - | - | 5 | 5 | 5 | 5 | 5 |
| **80** | - | 5 | 5 | 5 | 5 | 5 | 5 |
| **100** | 5 | 5 | 5 | 5 | 5 | - | - |

### Ohlmann-Thomas — 25 インスタンス

各 (n, w) 組に 5 インスタンス．最大規模．

| n \ w | 120 | 140 | 160 |
|---:|:---:|:---:|:---:|
| **150** | 5 | 5 | 5 |
| **200** | 5 | 5 | - |

### セット間の関係

| ベンチマークセット | 都市数 n | 時間枠幅 w | 特徴 |
|:---|:---|:---|:---|
| AFG | 10–231 | (構造化なし) | 非対称、幅広いサイズ |
| Dumas | 20–200 | 20–100 (狭い) | 対称、タイトな制約 |
| Gendreau-Dumas Ext. | 20–100 | 80–200 (広い) | 対称、緩い制約 |
| Ohlmann-Thomas | 150–200 | 120–160 | 対称、大規模 |

Dumas と Gendreau-Dumas Extended は n=20–100, w=80–100 の範囲で重複・補完関係にあり、合わせて w=20–200 の全範囲をカバーしている．Ohlmann-Thomas は大規模インスタンス (n=150–200) を追加している．

### Best Known Bounds (Travel time)

出典: [TSPTW Instances](https://lopez-ibanez.eu/tsptw-instances)（[Traveltime_Bounds.csv](https://lopez-ibanez.eu/files/TSPTW/Traveltime_Bounds.csv)，`benchmark/Traveltime_Bounds.csv` にコピーを保存）．

| ベンチマークセット | インスタンス数 | 最適証明済 | 未証明 | BKS 範囲 |
|:---|---:|---:|---:|:---|
| AFG | 50 | 46 | 4 | 671–14992 |
| Dumas | 135 | 135 | 0 | 222–1050 |
| Gendreau-Dumas Ext. | 130 | 130 | 0 | 176–700 |
| Ohlmann-Thomas | 25 | 24 | 1 | 608–880 |
| **合計** | **340** | **335** | **5** | |

未証明最適のインスタンス（open instances）:

| Instance | BKS | LB | Gap (%) | Source |
|:---|---:|---:|---:|:---|
| rbg049a.tw | 10018 | 10006.1 | 2.50 | Baldacci et al. (2012) |
| rbg050b.tw | 9863 | 9855.8 | 1.40 | Baldacci et al. (2012) |
| rbg050c.tw | 10024 | 10020.4 | 0.70 | Baldacci et al. (2012) |
| rbg193.2.tw | 12138 | 12136.3 | 0.01 | Baldacci et al. (2012) |
| n200w140.004.txt | 816 | 807.1 | 1.30 | Baldacci et al. (2012) |

#### 一問ごとの Best Known Bounds

BKS = Best Known Solution, LB = Lower Bound (Relaxed Bound)．Gap が optimal のインスタンスは最適性が証明済．

<details>
<summary>AFG（50 インスタンス）</summary>

| Instance | BKS | LB | Gap (%) | Source |
|:---|---:|---:|---:|:---|
| rbg010a.tw | 671 | 671 | optimal | Ascheuer et al. (2001) |
| rbg016a.tw | 938 | 938 | optimal | Ascheuer et al. (2001) |
| rbg016b.tw | 1304 | 1304 | optimal | Ascheuer et al. (2001) |
| rbg017.2.tw | 852 | 852 | optimal | Ascheuer et al. (2001) |
| rbg017.tw | 893 | 893 | optimal | Ascheuer et al. (2001) |
| rbg017a.tw | 4296 | 4296 | optimal | Ascheuer et al. (2001) |
| rbg019a.tw | 1262 | 1262 | optimal | Ascheuer et al. (2001) |
| rbg019b.tw | 1866 | 1866 | optimal | Ascheuer et al. (2001) |
| rbg019c.tw | 4536 | 4536 | optimal | Ascheuer et al. (2001) |
| rbg019d.tw | 1356 | 1356 | optimal | Ascheuer et al. (2001) |
| rbg020a.tw | 4689 | 4689 | optimal | Ascheuer et al. (2001) |
| rbg021.2.tw | 4528 | 4528 | optimal | Ascheuer et al. (2001) |
| rbg021.3.tw | 4528 | 4528 | optimal | Ascheuer et al. (2001) |
| rbg021.4.tw | 4525 | 4525 | optimal | Ascheuer et al. (2001) |
| rbg021.5.tw | 4515 | 4515 | optimal | Ascheuer et al. (2001) |
| rbg021.6.tw | 4480 | 4480 | optimal | Ascheuer et al. (2001) |
| rbg021.7.tw | 4479 | 4479 | optimal | Ascheuer et al. (2001) |
| rbg021.8.tw | 4478 | 4478 | optimal | Ascheuer et al. (2001) |
| rbg021.9.tw | 4478 | 4478 | optimal | Ascheuer et al. (2001) |
| rbg021.tw | 4536 | 4536 | optimal | Ascheuer et al. (2001) |
| rbg027a.tw | 5091 | 5091 | optimal | Ascheuer et al. (2001) |
| rbg031a.tw | 1863 | 1863 | optimal | Ascheuer et al. (2001) |
| rbg033a.tw | 2069 | 2069 | optimal | Ascheuer et al. (2001) |
| rbg034a.tw | 2222 | 2222 | optimal | Ascheuer et al. (2001) |
| rbg035a.2.tw | 2056 | 2056 | optimal | Ascheuer et al. (2001) |
| rbg035a.tw | 2144 | 2144 | optimal | Ascheuer et al. (2001) |
| rbg038a.tw | 2480 | 2480 | optimal | Ascheuer et al. (2001) |
| rbg040a.tw | 2378 | 2378 | optimal | Ascheuer et al. (2001) |
| rbg041a.tw | 2598 | 2598 | optimal | Focacci et al. (2002) |
| rbg042a.tw | 2772 | 2772 | optimal | Focacci et al. (2002) |
| rbg048a.tw | 9383 | 9383 | optimal | Dash et al. (2012) |
| rbg049a.tw | 10018 | 10006.1 | 2.50 | Baldacci et al. (2012) |
| rbg050a.tw | 2953 | 2953 | optimal | Ascheuer et al. (2001) |
| rbg050b.tw | 9863 | 9855.8 | 1.40 | Baldacci et al. (2012) |
| rbg050c.tw | 10024 | 10020.4 | 0.70 | Baldacci et al. (2012) |
| rbg055a.tw | 3761 | 3761 | optimal | Ascheuer et al. (2001) |
| rbg067a.tw | 4625 | 4625 | optimal | Ascheuer et al. (2001) |
| rbg086a.tw | 8400 | 8400 | optimal | Dash et al. (2012) |
| rbg092a.tw | 7158 | 7158 | optimal | Dash et al. (2012) |
| rbg125a.tw | 7936 | 7936 | optimal | Ascheuer et al. (2001) |
| rbg132.2.tw | 8191 | 8191 | optimal | Dash et al. (2012) |
| rbg132.tw | 8468 | 8468 | optimal | Dash et al. (2012) |
| rbg152.3.tw | 9788 | 9788 | optimal | Dash et al. (2012) |
| rbg152.tw | 10032 | 10032 | optimal | Dash et al. (2012) |
| rbg172a.tw | 10950 | 10950 | optimal | Dash et al. (2012) |
| rbg193.2.tw | 12138 | 12136.3 | 0.01 | Baldacci et al. (2012) |
| rbg193.tw | 12535 | 12535 | optimal | Dash et al. (2012) |
| rbg201a.tw | 12948 | 12948 | optimal | Dash et al. (2012) |
| rbg233.2.tw | 14492 | 14491 | optimal | Baldacci et al. (2012) |
| rbg233.tw | 14992 | 14992 | optimal | Dash et al. (2012) |

</details>

<details>
<summary>Dumas（135 インスタンス）</summary>

| Instance | BKS | LB | Gap (%) | Source |
|:---|---:|---:|---:|:---|
| n100w20.001.txt | 738 | 738 | optimal | Dumas et al. (1995) |
| n100w20.002.txt | 715 | 715 | optimal | Dumas et al. (1995) |
| n100w20.003.txt | 762 | 762 | optimal | Dumas et al. (1995) |
| n100w20.004.txt | 799 | 799 | optimal | Dumas et al. (1995) |
| n100w20.005.txt | 774 | 774 | optimal | Dumas et al. (1995) |
| n100w40.001.txt | 770 | 770 | optimal | Dumas et al. (1995) |
| n100w40.002.txt | 653 | 653 | optimal | Dumas et al. (1995) |
| n100w40.003.txt | 736 | 736 | optimal | Dumas et al. (1995) |
| n100w40.004.txt | 651 | 651 | optimal | Dumas et al. (1995) |
| n100w40.005.txt | 699 | 699 | optimal | Dumas et al. (1995) |
| n100w60.001.txt | 655 | 655 | optimal | Kuroiwa and Beck (2023a) |
| n100w60.002.txt | 659 | 659 | optimal | Dumas et al. (1995) |
| n100w60.003.txt | 744 | 744 | optimal | Kuroiwa and Beck (2023a) |
| n100w60.004.txt | 764 | 764 | optimal | Dumas et al. (1995) |
| n100w60.005.txt | 661 | 661 | optimal | Dumas et al. (1995) |
| n150w20.001.txt | 925 | 925 | optimal | Dumas et al. (1995) |
| n150w20.002.txt | 864 | 864 | optimal | Dumas et al. (1995) |
| n150w20.003.txt | 834 | 834 | optimal | Dumas et al. (1995) |
| n150w20.004.txt | 873 | 873 | optimal | Dumas et al. (1995) |
| n150w20.005.txt | 846 | 846 | optimal | Dumas et al. (1995) |
| n150w40.001.txt | 918 | 918 | optimal | Dumas et al. (1995) |
| n150w40.002.txt | 941 | 941 | optimal | Dumas et al. (1995) |
| n150w40.003.txt | 727 | 727 | optimal | Dumas et al. (1995) |
| n150w40.004.txt | 764 | 764 | optimal | Dumas et al. (1995) |
| n150w40.005.txt | 824 | 824 | optimal | Dumas et al. (1995) |
| n150w60.001.txt | 859 | 859 | optimal | Dumas et al. (1995) |
| n150w60.002.txt | 782 | 782 | optimal | Dumas et al. (1995) |
| n150w60.003.txt | 793 | 793 | optimal | Dumas et al. (1995) |
| n150w60.004.txt | 819 | 819 | optimal | Dumas et al. (1995) |
| n150w60.005.txt | 840 | 840 | optimal | Dumas et al. (1995) |
| n200w20.001.txt | 1019 | 1019 | optimal | Dumas et al. (1995) |
| n200w20.002.txt | 972 | 972 | optimal | Dumas et al. (1995) |
| n200w20.003.txt | 1050 | 1050 | optimal | Dumas et al. (1995) |
| n200w20.004.txt | 984 | 984 | optimal | Dumas et al. (1995) |
| n200w20.005.txt | 1020 | 1020 | optimal | Dumas et al. (1995) |
| n200w40.001.txt | 1023 | 1023 | optimal | Dumas et al. (1995) |
| n200w40.002.txt | 948 | 948 | optimal | Dumas et al. (1995) |
| n200w40.003.txt | 933 | 933 | optimal | Dumas et al. (1995) |
| n200w40.004.txt | 980 | 980 | optimal | Dumas et al. (1995) |
| n200w40.005.txt | 1037 | 1037 | optimal | Dumas et al. (1995) |
| n20w100.001.txt | 237 | 237 | optimal | Dumas et al. (1995) |
| n20w100.002.txt | 222 | 222 | optimal | Dumas et al. (1995) |
| n20w100.003.txt | 310 | 310 | optimal | Dumas et al. (1995) |
| n20w100.004.txt | 349 | 349 | optimal | Dumas et al. (1995) |
| n20w100.005.txt | 258 | 258 | optimal | Dumas et al. (1995) |
| n20w20.001.txt | 378 | 378 | optimal | Dumas et al. (1995) |
| n20w20.002.txt | 286 | 286 | optimal | Dumas et al. (1995) |
| n20w20.003.txt | 394 | 394 | optimal | Dumas et al. (1995) |
| n20w20.004.txt | 396 | 396 | optimal | Dumas et al. (1995) |
| n20w20.005.txt | 352 | 352 | optimal | Dumas et al. (1995) |
| n20w40.001.txt | 254 | 254 | optimal | Dumas et al. (1995) |
| n20w40.002.txt | 333 | 333 | optimal | Dumas et al. (1995) |
| n20w40.003.txt | 317 | 317 | optimal | Dumas et al. (1995) |
| n20w40.004.txt | 388 | 388 | optimal | Dumas et al. (1995) |
| n20w40.005.txt | 288 | 288 | optimal | Dumas et al. (1995) |
| n20w60.001.txt | 335 | 335 | optimal | Dumas et al. (1995) |
| n20w60.002.txt | 244 | 244 | optimal | Dumas et al. (1995) |
| n20w60.003.txt | 352 | 352 | optimal | Dumas et al. (1995) |
| n20w60.004.txt | 280 | 280 | optimal | Dumas et al. (1995) |
| n20w60.005.txt | 338 | 338 | optimal | Dumas et al. (1995) |
| n20w80.001.txt | 329 | 329 | optimal | Dumas et al. (1995) |
| n20w80.002.txt | 338 | 338 | optimal | Dumas et al. (1995) |
| n20w80.003.txt | 320 | 320 | optimal | Dumas et al. (1995) |
| n20w80.004.txt | 304 | 304 | optimal | Dumas et al. (1995) |
| n20w80.005.txt | 264 | 264 | optimal | Dumas et al. (1995) |
| n40w100.001.txt | 429 | 429 | optimal | Dumas et al. (1995) |
| n40w100.002.txt | 358 | 358 | optimal | Dumas et al. (1995) |
| n40w100.003.txt | 364 | 364 | optimal | Dumas et al. (1995) |
| n40w100.004.txt | 357 | 357 | optimal | Dumas et al. (1995) |
| n40w100.005.txt | 377 | 377 | optimal | Dumas et al. (1995) |
| n40w20.001.txt | 500 | 500 | optimal | Dumas et al. (1995) |
| n40w20.002.txt | 552 | 552 | optimal | Dumas et al. (1995) |
| n40w20.003.txt | 478 | 478 | optimal | Dumas et al. (1995) |
| n40w20.004.txt | 404 | 404 | optimal | Dumas et al. (1995) |
| n40w20.005.txt | 499 | 499 | optimal | Dumas et al. (1995) |
| n40w40.001.txt | 465 | 465 | optimal | Dumas et al. (1995) |
| n40w40.002.txt | 461 | 461 | optimal | Dumas et al. (1995) |
| n40w40.003.txt | 474 | 474 | optimal | Dumas et al. (1995) |
| n40w40.004.txt | 452 | 452 | optimal | Dumas et al. (1995) |
| n40w40.005.txt | 453 | 453 | optimal | Dumas et al. (1995) |
| n40w60.001.txt | 494 | 494 | optimal | Dumas et al. (1995) |
| n40w60.002.txt | 470 | 470 | optimal | Dumas et al. (1995) |
| n40w60.003.txt | 408 | 408 | optimal | Dumas et al. (1995) |
| n40w60.004.txt | 382 | 382 | optimal | Dumas et al. (1995) |
| n40w60.005.txt | 328 | 328 | optimal | Dumas et al. (1995) |
| n40w80.001.txt | 395 | 395 | optimal | Dumas et al. (1995) |
| n40w80.002.txt | 431 | 431 | optimal | Dumas et al. (1995) |
| n40w80.003.txt | 412 | 412 | optimal | Dumas et al. (1995) |
| n40w80.004.txt | 417 | 417 | optimal | Dumas et al. (1995) |
| n40w80.005.txt | 344 | 344 | optimal | Dumas et al. (1995) |
| n60w100.001.txt | 515 | 515 | optimal | Dumas et al. (1995) |
| n60w100.002.txt | 538 | 538 | optimal | Dumas et al. (1995) |
| n60w100.003.txt | 560 | 560 | optimal | Dumas et al. (1995) |
| n60w100.004.txt | 510 | 510 | optimal | Dumas et al. (1995) |
| n60w100.005.txt | 451 | 451 | optimal | Dumas et al. (1995) |
| n60w20.001.txt | 551 | 551 | optimal | Dumas et al. (1995) |
| n60w20.002.txt | 605 | 605 | optimal | Dumas et al. (1995) |
| n60w20.003.txt | 533 | 533 | optimal | Dumas et al. (1995) |
| n60w20.004.txt | 616 | 616 | optimal | Dumas et al. (1995) |
| n60w20.005.txt | 603 | 603 | optimal | Dumas et al. (1995) |
| n60w40.001.txt | 591 | 591 | optimal | Dumas et al. (1995) |
| n60w40.002.txt | 621 | 621 | optimal | Dumas et al. (1995) |
| n60w40.003.txt | 603 | 603 | optimal | Dumas et al. (1995) |
| n60w40.004.txt | 597 | 597 | optimal | Dumas et al. (1995) |
| n60w40.005.txt | 539 | 539 | optimal | Dumas et al. (1995) |
| n60w60.001.txt | 609 | 609 | optimal | Dumas et al. (1995) |
| n60w60.002.txt | 566 | 566 | optimal | Dumas et al. (1995) |
| n60w60.003.txt | 485 | 485 | optimal | Dumas et al. (1995) |
| n60w60.004.txt | 571 | 571 | optimal | Dumas et al. (1995) |
| n60w60.005.txt | 569 | 569 | optimal | Dumas et al. (1995) |
| n60w80.001.txt | 458 | 458 | optimal | Dumas et al. (1995) |
| n60w80.002.txt | 498 | 498 | optimal | Dumas et al. (1995) |
| n60w80.003.txt | 550 | 550 | optimal | Dumas et al. (1995) |
| n60w80.004.txt | 566 | 566 | optimal | Dumas et al. (1995) |
| n60w80.005.txt | 468 | 468 | optimal | Dumas et al. (1995) |
| n80w20.001.txt | 616 | 616 | optimal | Dumas et al. (1995) |
| n80w20.002.txt | 737 | 737 | optimal | Dumas et al. (1995) |
| n80w20.003.txt | 667 | 667 | optimal | Dumas et al. (1995) |
| n80w20.004.txt | 615 | 615 | optimal | Dumas et al. (1995) |
| n80w20.005.txt | 748 | 748 | optimal | Dumas et al. (1995) |
| n80w40.001.txt | 606 | 606 | optimal | Dumas et al. (1995) |
| n80w40.002.txt | 618 | 618 | optimal | Dumas et al. (1995) |
| n80w40.003.txt | 674 | 674 | optimal | Dumas et al. (1995) |
| n80w40.004.txt | 557 | 557 | optimal | Dumas et al. (1995) |
| n80w40.005.txt | 695 | 695 | optimal | Dumas et al. (1995) |
| n80w60.001.txt | 554 | 554 | optimal | Dumas et al. (1995) |
| n80w60.002.txt | 633 | 633 | optimal | Dumas et al. (1995) |
| n80w60.003.txt | 651 | 651 | optimal | Dumas et al. (1995) |
| n80w60.004.txt | 619 | 619 | optimal | Dumas et al. (1995) |
| n80w60.005.txt | 575 | 575 | optimal | Dumas et al. (1995) |
| n80w80.001.txt | 624 | 624 | optimal | Dumas et al. (1995) |
| n80w80.002.txt | 592 | 592 | optimal | Dumas et al. (1995) |
| n80w80.003.txt | 589 | 589 | optimal | Dumas et al. (1995) |
| n80w80.004.txt | 594 | 594 | optimal | Dumas et al. (1995) |
| n80w80.005.txt | 570 | 570 | optimal | Dumas et al. (1995) |

</details>

<details>
<summary>Gendreau-Dumas Ext.（130 インスタンス）</summary>

| Instance | BKS | LB | Gap (%) | Source |
|:---|---:|---:|---:|:---|
| n100w100.001.txt | 643 | 643 | optimal | Baldacci et al. (2012) |
| n100w100.002.txt | 619 | 618 | optimal | Baldacci et al. (2012) |
| n100w100.003.txt | 685 | 685 | optimal | Baldacci et al. (2012) |
| n100w100.004.txt | 684 | 684 | optimal | Baldacci et al. (2012) |
| n100w100.005.txt | 572 | 572 | optimal | Baldacci et al. (2012) |
| n100w120.001.txt | 629 | 629 | optimal | Baldacci et al. (2012) |
| n100w120.002.txt | 540 | 540 | optimal | Baldacci et al. (2012) |
| n100w120.003.txt | 617 | 615 | optimal | Baldacci et al. (2012) |
| n100w120.004.txt | 663 | 662 | optimal | Baldacci et al. (2012) |
| n100w120.005.txt | 537 | 537 | optimal | Baldacci et al. (2012) |
| n100w140.001.txt | 604 | 603 | optimal | Baldacci et al. (2012) |
| n100w140.002.txt | 615 | 613 | optimal | Baldacci et al. (2012) |
| n100w140.003.txt | 481 | 481 | optimal | Baldacci et al. (2012) |
| n100w140.004.txt | 533 | 533 | optimal | Baldacci et al. (2012) |
| n100w140.005.txt | 509 | 509 | optimal | Baldacci et al. (2012) |
| n100w160.001.txt | 582 | 582 | optimal | Baldacci et al. (2012) |
| n100w160.002.txt | 532 | 530 | optimal | Baldacci et al. (2012) |
| n100w160.003.txt | 495 | 495 | optimal | Baldacci et al. (2012) |
| n100w160.004.txt | 580 | 580 | optimal | Baldacci et al. (2012) |
| n100w160.005.txt | 586 | 586 | optimal | Baldacci et al. (2012) |
| n100w80.001.txt | 670 | 670 | optimal | Baldacci et al. (2012) |
| n100w80.002.txt | 668 | 666 | optimal | Baldacci et al. (2012) |
| n100w80.003.txt | 691 | 691 | optimal | Baldacci et al. (2012) |
| n100w80.004.txt | 700 | 700 | optimal | Baldacci et al. (2012) |
| n100w80.005.txt | 603 | 603 | optimal | Baldacci et al. (2012) |
| n20w120.001.txt | 267 | 267 | optimal | Baldacci et al. (2012) |
| n20w120.002.txt | 218 | 218 | optimal | Baldacci et al. (2012) |
| n20w120.003.txt | 303 | 303 | optimal | Baldacci et al. (2012) |
| n20w120.004.txt | 300 | 300 | optimal | Baldacci et al. (2012) |
| n20w120.005.txt | 240 | 240 | optimal | Baldacci et al. (2012) |
| n20w140.001.txt | 176 | 176 | optimal | Baldacci et al. (2012) |
| n20w140.002.txt | 272 | 272 | optimal | Baldacci et al. (2012) |
| n20w140.003.txt | 236 | 236 | optimal | Baldacci et al. (2012) |
| n20w140.004.txt | 255 | 255 | optimal | Baldacci et al. (2012) |
| n20w140.005.txt | 225 | 225 | optimal | Baldacci et al. (2012) |
| n20w160.001.txt | 241 | 241 | optimal | Baldacci et al. (2012) |
| n20w160.002.txt | 201 | 201 | optimal | Baldacci et al. (2012) |
| n20w160.003.txt | 201 | 201 | optimal | Baldacci et al. (2012) |
| n20w160.004.txt | 203 | 203 | optimal | Baldacci et al. (2012) |
| n20w160.005.txt | 245 | 245 | optimal | Baldacci et al. (2012) |
| n20w180.001.txt | 253 | 253 | optimal | Baldacci et al. (2012) |
| n20w180.002.txt | 265 | 265 | optimal | Baldacci et al. (2012) |
| n20w180.003.txt | 271 | 271 | optimal | Baldacci et al. (2012) |
| n20w180.004.txt | 201 | 201 | optimal | Baldacci et al. (2012) |
| n20w180.005.txt | 193 | 193 | optimal | Baldacci et al. (2012) |
| n20w200.001.txt | 233 | 233 | optimal | Baldacci et al. (2012) |
| n20w200.002.txt | 203 | 203 | optimal | Baldacci et al. (2012) |
| n20w200.003.txt | 249 | 249 | optimal | Baldacci et al. (2012) |
| n20w200.004.txt | 293 | 293 | optimal | Rudich et al. (2023) |
| n20w200.005.txt | 227 | 227 | optimal | Baldacci et al. (2012) |
| n40w120.001.txt | 434 | 434 | optimal | Baldacci et al. (2012) |
| n40w120.002.txt | 445 | 445 | optimal | Rudich et al. (2023) |
| n40w120.003.txt | 357 | 357 | optimal | Baldacci et al. (2012) |
| n40w120.004.txt | 303 | 303 | optimal | Baldacci et al. (2012) |
| n40w120.005.txt | 350 | 350 | optimal | Baldacci et al. (2012) |
| n40w140.001.txt | 328 | 328 | optimal | Baldacci et al. (2012) |
| n40w140.002.txt | 383 | 383 | optimal | Baldacci et al. (2012) |
| n40w140.003.txt | 398 | 398 | optimal | Baldacci et al. (2012) |
| n40w140.004.txt | 342 | 342 | optimal | Baldacci et al. (2012) |
| n40w140.005.txt | 371 | 371 | optimal | Baldacci et al. (2012) |
| n40w160.001.txt | 348 | 348 | optimal | Baldacci et al. (2012) |
| n40w160.002.txt | 337 | 337 | optimal | Baldacci et al. (2012) |
| n40w160.003.txt | 346 | 346 | optimal | Baldacci et al. (2012) |
| n40w160.004.txt | 288 | 288 | optimal | Baldacci et al. (2012) |
| n40w160.005.txt | 315 | 315 | optimal | Baldacci et al. (2012) |
| n40w180.001.txt | 337 | 337 | optimal | Baldacci et al. (2012) |
| n40w180.002.txt | 347 | 347 | optimal | Baldacci et al. (2012) |
| n40w180.003.txt | 279 | 279 | optimal | Baldacci et al. (2012) |
| n40w180.004.txt | 354 | 354 | optimal | Baldacci et al. (2012) |
| n40w180.005.txt | 335 | 335 | optimal | Baldacci et al. (2012) |
| n40w200.001.txt | 330 | 330 | optimal | Baldacci et al. (2012) |
| n40w200.002.txt | 303 | 303 | optimal | Rudich et al. (2023) |
| n40w200.003.txt | 339 | 339 | optimal | Baldacci et al. (2012) |
| n40w200.004.txt | 301 | 301 | optimal | Baldacci et al. (2012) |
| n40w200.005.txt | 296 | 296 | optimal | Baldacci et al. (2012) |
| n60w120.001.txt | 384 | 384 | optimal | Baldacci et al. (2012) |
| n60w120.002.txt | 427 | 426 | optimal | Baldacci et al. (2012) |
| n60w120.003.txt | 407 | 407 | optimal | Baldacci et al. (2012) |
| n60w120.004.txt | 490 | 490 | optimal | Baldacci et al. (2012) |
| n60w120.005.txt | 547 | 547 | optimal | Baldacci et al. (2012) |
| n60w140.001.txt | 423 | 423 | optimal | Baldacci et al. (2012) |
| n60w140.002.txt | 462 | 462 | optimal | Baldacci et al. (2012) |
| n60w140.003.txt | 427 | 427 | optimal | Baldacci et al. (2012) |
| n60w140.004.txt | 488 | 488 | optimal | Baldacci et al. (2012) |
| n60w140.005.txt | 460 | 460 | optimal | Baldacci et al. (2012) |
| n60w160.001.txt | 560 | 560 | optimal | Baldacci et al. (2012) |
| n60w160.002.txt | 423 | 423 | optimal | Baldacci et al. (2012) |
| n60w160.003.txt | 434 | 434 | optimal | Baldacci et al. (2012) |
| n60w160.004.txt | 401 | 401 | optimal | Baldacci et al. (2012) |
| n60w160.005.txt | 502 | 501 | optimal | Baldacci et al. (2012) |
| n60w180.001.txt | 411 | 411 | optimal | Baldacci et al. (2012) |
| n60w180.002.txt | 399 | 399 | optimal | Baldacci et al. (2012) |
| n60w180.003.txt | 445 | 444 | optimal | Baldacci et al. (2012) |
| n60w180.004.txt | 456 | 456 | optimal | Baldacci et al. (2012) |
| n60w180.005.txt | 395 | 395 | optimal | Baldacci et al. (2012) |
| n60w200.001.txt | 410 | 410 | optimal | Baldacci et al. (2012) |
| n60w200.002.txt | 414 | 414 | optimal | Baldacci et al. (2012) |
| n60w200.003.txt | 455 | 455 | optimal | Baldacci et al. (2012) |
| n60w200.004.txt | 431 | 431 | optimal | Baldacci et al. (2012) |
| n60w200.005.txt | 427 | 427 | optimal | Baldacci et al. (2012) |
| n80w100.001.txt | 565 | 541 | optimal | Baldacci et al. (2012) |
| n80w100.002.txt | 567 | 567 | optimal | Baldacci et al. (2012) |
| n80w100.003.txt | 580 | 578 | optimal | Baldacci et al. (2012) |
| n80w100.004.txt | 649 | 649 | optimal | Rudich et al. (2023) |
| n80w100.005.txt | 532 | 532 | optimal | Baldacci et al. (2012) |
| n80w120.001.txt | 498 | 498 | optimal | Baldacci et al. (2012) |
| n80w120.002.txt | 577 | 577 | optimal | Baldacci et al. (2012) |
| n80w120.003.txt | 540 | 540 | optimal | Baldacci et al. (2012) |
| n80w120.004.txt | 501 | 501 | optimal | Baldacci et al. (2012) |
| n80w120.005.txt | 591 | 591 | optimal | Baldacci et al. (2012) |
| n80w140.001.txt | 512 | 511 | optimal | Baldacci et al. (2012) |
| n80w140.002.txt | 470 | 470 | optimal | Baldacci et al. (2012) |
| n80w140.003.txt | 580 | 580 | optimal | Baldacci et al. (2012) |
| n80w140.004.txt | 423 | 422 | optimal | Baldacci et al. (2012) |
| n80w140.005.txt | 545 | 545 | optimal | Baldacci et al. (2012) |
| n80w160.001.txt | 506 | 506 | optimal | Baldacci et al. (2012) |
| n80w160.002.txt | 549 | 548 | optimal | Baldacci et al. (2012) |
| n80w160.003.txt | 521 | 521 | optimal | Baldacci et al. (2012) |
| n80w160.004.txt | 509 | 509 | optimal | Baldacci et al. (2012) |
| n80w160.005.txt | 439 | 438 | optimal | Baldacci et al. (2012) |
| n80w180.001.txt | 551 | 551 | optimal | Baldacci et al. (2012) |
| n80w180.002.txt | 479 | 478 | optimal | Baldacci et al. (2012) |
| n80w180.003.txt | 524 | 524 | optimal | Baldacci et al. (2012) |
| n80w180.004.txt | 479 | 479 | optimal | Baldacci et al. (2012) |
| n80w180.005.txt | 470 | 470 | optimal | Baldacci et al. (2012) |
| n80w200.001.txt | 490 | 490 | optimal | Baldacci et al. (2012) |
| n80w200.002.txt | 488 | 488 | optimal | Baldacci et al. (2012) |
| n80w200.003.txt | 464 | 464 | optimal | Baldacci et al. (2012) |
| n80w200.004.txt | 526 | 526 | optimal | Baldacci et al. (2012) |
| n80w200.005.txt | 439 | 439 | optimal | Baldacci et al. (2012) |

</details>

<details>
<summary>Ohlmann-Thomas（25 インスタンス）</summary>

| Instance | BKS | LB | Gap (%) | Source |
|:---|---:|---:|---:|:---|
| n150w120.001.txt | 734 | 732 | optimal | Baldacci et al. (2012) |
| n150w120.002.txt | 677 | 677 | optimal | Baldacci et al. (2012) |
| n150w120.003.txt | 747 | 747 | optimal | Baldacci et al. (2012) |
| n150w120.004.txt | 763 | 762 | optimal | Baldacci et al. (2012) |
| n150w120.005.txt | 689 | 689 | optimal | Baldacci et al. (2012) |
| n150w140.001.txt | 762 | 762 | optimal | Baldacci et al. (2012) |
| n150w140.002.txt | 755 | 753 | optimal | Baldacci et al. (2012) |
| n150w140.003.txt | 613 | 613 | optimal | Baldacci et al. (2012) |
| n150w140.004.txt | 676 | 676 | optimal | Baldacci et al. (2012) |
| n150w140.005.txt | 663 | 663 | optimal | Baldacci et al. (2012) |
| n150w160.001.txt | 706 | 704 | optimal | Baldacci et al. (2012) |
| n150w160.002.txt | 711 | 711 | optimal | Baldacci et al. (2012) |
| n150w160.003.txt | 608 | 608 | optimal | Baldacci et al. (2012) |
| n150w160.004.txt | 672 | 672 | optimal | Baldacci et al. (2012) |
| n150w160.005.txt | 658 | 658 | optimal | Baldacci et al. (2012) |
| n200w120.001.txt | 799 | 795 | optimal | Baldacci et al. (2012) |
| n200w120.002.txt | 721 | 721 | optimal | Baldacci et al. (2012) |
| n200w120.003.txt | 880 | 879 | optimal | Baldacci et al. (2012) |
| n200w120.004.txt | 777 | 777 | optimal | Baldacci et al. (2012) |
| n200w120.005.txt | 841 | 840 | optimal | Baldacci et al. (2012) |
| n200w140.001.txt | 834 | 830 | optimal | Baldacci et al. (2012) |
| n200w140.002.txt | 760 | 760 | optimal | Baldacci et al. (2012) |
| n200w140.003.txt | 758 | 758 | optimal | Baldacci et al. (2012) |
| n200w140.004.txt | 816 | 807.1 | 1.30 | Baldacci et al. (2012) |
| n200w140.005.txt | 822 | 822 | optimal | Baldacci et al. (2012) |

</details>
