# pxt4 / jbd3 생성기 (7.2.8)

pxt4는 ScaleCache page cache를 쓰는 ext4이고, jbd3는 pxt4용 jbd2입니다. 둘 다 커널 트리 밖 모듈입니다.
6.6.8 스냅샷의 `pxt4/`를 손으로 고치지 않고, 7.2.8 `fs/ext4`·`fs/jbd2`에서 이름을 바꾼 뒤 ScaleCache 변경만 patch로 다시 얹습니다.

```
./gen.sh <linux 저장소> v7.2.8 <출력>                    # 생성: <출력>/pxt4, <출력>/pxt4/jbd3
make -C <출력>/pxt4 KDIR=<ScaleCache 커널 빌드 디렉터리>   # pxt4.ko, jbd3/jbd3.ko
insmod jbd3/jbd3.ko && insmod pxt4.ko && mount -t pxt4 <dev> <dir>
```

- 커널은 ScaleCache 7.2.8 커널이어야 합니다: cc-core(cc_xarray), pxt4-support(jbd3·pxt4 헤더, export), dflush(`balance_dirty_pages_module`, writeback export).
- 원본 README처럼 jbd3를 따로 빌드하고 `Module.symvers`를 복사할 필요는 없습니다. 한 번의 빌드에서 두 모듈을 만듭니다.
- 커널 빌드 디렉터리에는 `make modules_prepare`(`scripts/module.lds`)와 `Module.symvers`가 있어야 합니다. vmlinux만 빌드한 디렉터리라면 `vmlinux.symvers`를 `Module.symvers`로 복사합니다.

## 브랜치 `sc-7.2.8-pxt4`

- origin의 부모 없는 브랜치에 생성기(`gen.sh`, `patches/`, 이 README)와 생성 결과(`pxt4/`)를 함께 둡니다.
- `pxt4/`는 `./gen.sh <linux 저장소> v7.2.8 <출력>`의 결과와 같아야 합니다. `pxt4/`를 직접 고치지 않고, patch를 고친 뒤 다시 생성합니다.
  - 확인: `./gen.sh ../linux v7.2.8 /tmp/pxg && diff -r /tmp/pxg/pxt4 pxt4`
- 시험한 커널: `sc-7.2.8-integ` + PR #7–#12. 프로젝트 config(`sc-7.2.8-config`)와 `CONFIG_CPUSETS_V1=y`에서 warning 0, QEMU 디버그 커널(KASAN, lockdep)에서 런타임 시험을 했습니다.

## patch 고치기

`gen.sh -n`은 이름 바꾸기만 하고 멈춥니다. 그 결과를 git 저장소로 만들고 patch를 커밋으로 얹은 뒤, 고쳐서 다시 내보냅니다.

```
./gen.sh -n ../linux v7.2.8 /tmp/pxb && cd /tmp/pxb/pxt4
git init -q && git add -A && git commit -qm base && git tag base
git am <이 브랜치>/patches/*.patch          # patch 하나가 커밋 하나
# 고친 뒤
git format-patch -o <이 브랜치>/patches base
```

## 이름 바꾸기

- pxt4-support 헤더와 같은 규칙입니다: `jbd2→jbd3, JBD2→JBD3, ext4→pxt4, EXT4→PXT4, Ext4→Pxt4, journal-head.h→journal-head3.h`. 파일 이름도 같습니다.
- 추가 규칙이 하나 있습니다. `pxt4.h`의 `EXTN_FEATURE_FUNCS(ver)`는 `EXT##ver##`·`ext##ver##`를 붙여 이름을 만들기 때문에, 2·3도 4와 같은 `PXT`·`pxt` 접두사로 맞춥니다.
  - 6.6.8 원본도 `PXT##ver##`로 바꾸고 `super.c`의 호출을 손으로 맞췄습니다.
- 바꾸지 않는 것
  - `ext2`·`ext3` 이름. pxt4는 ext4처럼 `ext3` 등록을 시도하고, ext4가 이미 등록했으면 `PXT4-fs: Unable to register as ext3 (-16)` 경고만 냅니다. 6.6.8과 같습니다.
  - `CONFIG_EXT4_USE_FOR_EXT2`는 정의하지 않습니다. pxt4는 ext2로 등록하지 않습니다.

## patch (`patches/`, 순서대로 적용)

| patch | 내용 | 6.6.8과의 차이 |
|---|---|---|
| 0001 build out of tree | 두 모듈을 한 번에 빌드. CONFIG_PXT4_FS_POSIX_ACL / _SECURITY / _DEBUG는 커널의 ext4 설정을 따름. Kconfig 삭제 | 6.6.8은 acl.o를 빌드했지만 `CONFIG_PXT4_FS_POSIX_ACL`을 정의하지 않아 ACL mount 옵션과 기본값이 빠져 있었음 |
| 0002 slab cache names | `KMEM_CACHE()`가 구조체 이름을 쓰는 cache 3개(`extent_status`, `pending_reservation`, `bio_post_read_ctx`)를 `pxt4_` 이름으로 | 6.6.8도 ext4와 이름이 겹침(DEBUG_VM이면 load 때 WARNING) |
| 0003 cc_xarray page cache | inode cache 생성자에서 `inode_init_once()` 뒤 `i_pages`를 `cc_xa_init_flags(…, CC_XA_FLAGS_ACCOUNT)` | 6.6.8은 `inode_init_once()` 복사본을 썼고 `nr_lock` 초기화가 빠져 있었음, inode마다 printk |
| 0004 dflush | `mm/page-writeback.c`: 7.2.8 `balance_dirty_pages()`와 static helper 복사 + pause 뒤 `wb_do_writeback_modified()`. 등록이 성공한 뒤 hook 설정 | 6.6.8 사본은 6.6.8 본문, `mm/internal.h` 사본 포함. hook을 init 처음에 걸고 실패해도 남김 |
| 0005 order 0, DAX 없음 | `s_max_folio_order` 0, page보다 큰 block size 거부, `dax`/`dax=always`/`dax=inode` 거부, cc 매핑은 DAX 안 씀 | 6.6.8 ext4에는 large folio가 없었음 |

## 7.2.8 커널과 맞춰야 하는 것

- `mm/page-writeback.c`의 `df_balance_dirty_pages()`는 커널 `balance_dirty_pages()`의 사본입니다. 커널을 올리면 다시 맞춥니다.
- `wb_dirty_limits()`는 dflush가 export하지만 `<linux/writeback.h>`에 선언이 없어 `mm/dflush.h`에 선언했습니다.

## 알려진 한계 (6.6.8부터)

- hook은 전역입니다. pxt4가 로드되어 있으면 다른 파일 시스템의 `balance_dirty_pages()`도 사본을 탑니다.
- 그래서 다른 파일 시스템이 throttle 중일 때 pxt4를 내리면(rmmod), 모듈 코드 안에서 잠든 task가 있을 수 있습니다. pxt4를 내릴 때는 쓰기가 멈춘 뒤에 내립니다.
