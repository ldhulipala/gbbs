include ../makefile.variables

INCLUDE_DIRS = -I../ -I../external/parlaylib/include/

OBJDIR = ../bin/gbbs/

ALL_PRE = benchmark bridge edge_map_blocked graph_io get_time io parse_command_line undirected_edge union_find vertex_subset
ALL= $(addprefix $(OBJDIR), $(addsuffix .o, $(ALL_PRE))) $(addprefix $(OBJDIR), $(addsuffix .a, $(ALL_PRE)))

all: $(ALL)

$(OBJDIR)bridge.o : bridge.cc
	$(CC) $(INCLUDE_DIRS) $(CFLAGS) $(PFLAGS) -c $< -o $@

$(OBJDIR)edge_map_blocked.o : edge_map_blocked.cc $(addprefix $(OBJDIR), bridge.o)
	$(CC) $(INCLUDE_DIRS) $(CFLAGS) $(PFLAGS) -c $< -o $@

$(OBJDIR)graph_io.o : graph_io.cc $(addprefix $(OBJDIR), io.o)
	$(CC) $(INCLUDE_DIRS) $(CFLAGS) $(PFLAGS) -c $< -o $@

$(OBJDIR)get_time.o : get_time.cc $(addprefix $(OBJDIR), io.o)
	$(CC) $(INCLUDE_DIRS) $(CFLAGS) $(PFLAGS) -c $< -o $@

$(OBJDIR)%.o : %.cc
	$(CC) $(INCLUDE_DIRS) $(CFLAGS) $(PFLAGS) -c $< -o $@

$(OBJDIR)%.a : $(OBJDIR)%.o
	ar -rcs $@ $<

.PHONY : clean

clean :
	rm -f *.o $(ALL)
